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
// cached under ~/.cache/omaroll/pdftext as compressed UTF-8, keyed by path,
// size and mtime, so a document is read once per version. A document that
// times out is not tried again this session until it changes.
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

signals:
  void textReady(const QString& path, const QString& text);
  void indexingChanged();

private:
  struct Candidate {
    QString path;
    qint64 modified = 0;
    qint64 bytes = 0;
  };
  struct Result {
    Candidate candidate;
    QString text;
    bool read = false;
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
