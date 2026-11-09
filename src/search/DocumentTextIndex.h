#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>

class CaptureModel;

// Searchable text of the library's PDFs, from pdftotext.
//
// Works like the picture text index: nothing runs until a search has text
// to look for, or background text indexing is on and the library has
// settled. Documents are read one at a time on a worker thread, newest
// first, with the first pages and a bounded amount of text kept. Results are
// cached under ~/.cache/omaroll/pdftext as compressed UTF-8, keyed by path and
// by size, mtime and ctime, so a document is read once per version, and a
// replacement that keeps the size and mtime is still read again. A document
// that times out is not tried again this session until it changes. Background
// indexing pauses on battery, as picture text does; a search still reads.
//
// The cache also backs search: text the library keeps on disk rather than in
// memory is read back from here (cachedText()).
class DocumentTextIndex final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool indexing READ indexing NOTIFY indexingChanged)

public:
  static constexpr int kPageLimit = 50;
  // Characters kept per document. Fifty dense pages fit; the rest of a book
  // is not searched.
  static constexpr qsizetype kTextLimit = 256 * 1024;
  static constexpr int kDefaultTimeoutMs = 20'000;

  explicit DocumentTextIndex(CaptureModel* model, QObject* parent = nullptr);
  ~DocumentTextIndex() override;
  void requestStop();

  [[nodiscard]] bool indexing() const { return m_indexing; }
  void setSearchText(const QString& text);
  void setBackground(bool enabled);
  // Forgets every document's text and removes the cache. Returns how many
  // documents were forgotten.
  Q_INVOKABLE int clearCache();
  // How long one document may take. Tests shorten it.
  void setTimeoutMs(int milliseconds) { m_timeoutMs = milliseconds; }

  [[nodiscard]] static QString cacheDirectory();
  // A document's cached text, or a null string when the cache has none for
  // the file as it is now. Safe on any thread.
  [[nodiscard]] static QString cachedText(const QString& path);
#ifdef OMAROLL_FUZZ
  // Reads |file| as a cache entry, with and without its text.
  static void fuzzReadCache(const QString& file);
#endif
  // Reads a document again, for text that went missing from the cache.
  void reread(const QString& path);

  // Where power supplies are read, for tests, and reading them now.
  void setPowerSupplyDirectory(const QString& directory);
  void checkPower();
  [[nodiscard]] bool onBattery() const { return m_onBattery; }

signals:
  // |persisted| says the text is in the cache, so cachedText() can give it
  // back later.
  void textReady(const QString& path, const QString& text, bool persisted);
  void indexingChanged();

private:
  struct Candidate {
    QString path;
    qint64 modified = 0;
    qint64 bytes = 0;
    // The scanner's full file identity, ctime and inode included.
    QString version;
    [[nodiscard]] bool sameVersion(const Candidate& other) const {
      return modified == other.modified && bytes == other.bytes && version == other.version;
    }
  };
  struct Result {
    Candidate candidate;
    QString text;
    bool read = false;
    bool persisted = false;
    quint64 generation = 0;
  };

  [[nodiscard]] bool wanted() const;
  void sync();
  void processNext();
  void finished();
  [[nodiscard]] bool known(const Candidate& candidate) const;
  [[nodiscard]] bool current(const Candidate& candidate) const;
  static Result extract(const Candidate& candidate, const QString& program,
                        const QString& cachePath, int timeoutMs, const std::atomic_bool& cancel);
  [[nodiscard]] static QString cachePath(const QString& path);
  static void pruneCache();
  void setIndexing(bool value);

  CaptureModel* m_model = nullptr;
  QHash<QString, Candidate> m_known;
  // Documents that could not be read (timed out, or no pdftotext), by
  // version, so each sync does not spend the timeout on them again.
  QHash<QString, Candidate> m_failed;
  int m_timeoutMs = kDefaultTimeoutMs;
  QList<Candidate> m_queue;
  QFutureWatcher<Result> m_watcher;
  QTimer m_syncTimer;
  QTimer m_powerTimer;
  QString m_powerDirectory = QStringLiteral("/sys/class/power_supply");
  bool m_onBattery = false;
  // Set on teardown so a document being read stops at once.
  std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
  // The document being read, and which cache it belongs to. Clearing the
  // cache starts a new generation, so a read in flight is discarded.
  Candidate m_current;
  quint64 m_generation = 0;
  bool m_running = false;
  bool m_searching = false;
  bool m_background = false;
  bool m_stopped = false;
  bool m_indexing = false;
};
