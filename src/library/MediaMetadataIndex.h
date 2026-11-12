#pragma once

#include <optional>

#include "library/CaptureModel.h"
#include "library/EmbeddedMetadata.h"

#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <memory>
#include <vector>

// Reads embedded metadata from general photos and videos: the original
// capture date, and the camera and lens that took them.
//
// Discovery itself stays a fast filesystem walk. Once a scan lands, this
// class checks small image batches with ImageMagick and videos one at a time
// with ffprobe, a few at once at low priority, then replaces only mtime
// fallbacks. Probe output is parsed on a worker; the window thread only
// adopts the parsed results. Omarchy's timestamped
// capture names always remain authoritative. Results, including "nothing
// embedded", are cached by path and filesystem identity so an unchanged
// library is not probed every launch.
//
// The same workers read keywords, a star rating and a description that other
// photo tools keep in the file or an .xmp sidecar (EmbeddedMetadata), and hand
// them to AppSettings as a read-only layer. That data has its own cache file,
// keyed by the file's identity and its sidecar's stamp, so an unchanged file
// is never read again and keyword data never crowds the date cache.
class MediaMetadataIndex final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool indexing READ indexing NOTIFY indexingChanged)
  Q_PROPERTY(int completed READ completed NOTIFY progressChanged)
  Q_PROPERTY(int total READ total NOTIFY progressChanged)

public:
  // What one probe found. Empty strings and an invalid date mean the file
  // carries nothing, which is still worth caching.
  struct Details {
    QDateTime captured;
    QString camera;
    QString lens;

    bool operator==(const Details& other) const = default;
  };

  explicit MediaMetadataIndex(CaptureModel* model, QObject* parent = nullptr);
  ~MediaMetadataIndex() override;
  void requestStop();

  [[nodiscard]] bool indexing() const { return m_indexing; }
  [[nodiscard]] int completed() const { return m_completed; }
  [[nodiscard]] int total() const { return m_total; }

  [[nodiscard]] static QString cachePath();
  // Keywords, ratings and descriptions read from files and sidecars.
  [[nodiscard]] static QString embeddedCachePath();
  // The embedded cache's own cap. Descriptions go first, then the entries
  // seen longest ago.
  static constexpr qint64 kMaximumEmbeddedCacheBytes = 64LL * 1024 * 1024;
  // Results reach AppSettings at most this often while indexing.
  static constexpr int kEmbeddedIntervalMs = 1000;
  // In-process reads run in batches this size.
  static constexpr int kEmbeddedBatchSize = 64;
#ifdef OMAROLL_TESTING
  // Reads of file data that ran on the window thread. Always zero.
  static std::atomic<int> s_embeddedReadsOnWindowThread;
  static std::atomic<int> s_embeddedReads;
  // Fills the embedded cache without reading files, then saves both caches.
  void insertEmbeddedForTesting(const QString& path, const EmbeddedMetadata::Fields& fields);
  void saveCachesForTesting() { m_cacheDirty = true; m_embeddedDirty = true; saveCache(); }
  [[nodiscard]] int embeddedCacheSize() const { return static_cast<int>(m_embedded.size()); }
  // A read that finished for |path| and waits for the next delivery, then
  // the sync a scan starts.
  void adoptEmbeddedForTesting(const QString& path, const EmbeddedMetadata::Fields& fields) {
    Candidate candidate;
    candidate.path = path;
    adoptEmbedded(candidate, fields);
  }
  void syncForTesting() { sync(); }
  // Marks one cached file as the cap trims it on disk.
  void markEmbeddedPartialForTesting(const QString& path) {
    if (const auto it = m_embedded.find(path); it != m_embedded.end()) {
      it->partial = true;
      m_embeddedDirty = true;
    }
  }
  // Description and the read-again flag of one cached file, or nullopt.
  [[nodiscard]] std::optional<std::pair<QString, bool>> embeddedEntryForTesting(const QString& path) const {
    const auto it = m_embedded.constFind(path);
    if (it == m_embedded.cend()) return std::nullopt;
    return std::pair{it->fields.description, it->partial};
  }
#endif
  // How many probes may run at once on this machine: 2 to 4.
  [[nodiscard]] static int maximumParallelProbes();
  // Progress is published at most this often while probes finish.
  static constexpr int kProgressIntervalMs = 1000;
  [[nodiscard]] static QDateTime parseImageDate(const QByteArray& output);
  // One EXIF date field. parseExifDate reads the usual shapes directly and
  // defers anything else to parseExifDateGeneral, so both always agree.
  [[nodiscard]] static QDateTime parseExifDate(const QString& value);
  [[nodiscard]] static QDateTime parseExifDateGeneral(const QString& value);
  [[nodiscard]] static Details parseImageDetails(const QByteArray& output);
  [[nodiscard]] static QDateTime parseVideoDate(const QByteArray& output);
  [[nodiscard]] static Details parseVideoDetails(const QByteArray& output);
  // Camera raws are read in-process: ImageMagick reports no EXIF for them.
  [[nodiscard]] static Details rawDetails(const QString& path);
  // "Apple iPhone 12", or just the model when the maker already leads it.
  [[nodiscard]] static QString cameraName(const QString& make, const QString& model);

signals:
  void indexingChanged();
  void progressChanged();

private:
  struct Candidate {
    QString path;
    qint64 modified = 0;
    qint64 bytes = 0;
    bool video = false;
    quint64 device = 0;
    quint64 inode = 0;
    bool raw = false;
    // The date probe is needed, and the file's own data is still unread.
    bool needsDate = true;
    bool needsEmbedded = false;
    QString sidecar;
    qint64 sidecarModified = 0;
    qint64 sidecarBytes = -1;
  };

  struct Probe;
  struct Parsed {
    QHash<int, Details> results;
    QSet<int> completed;
    QHash<int, EmbeddedMetadata::Fields> embedded;
  };

  // What a file and its sidecar held, for the file and sidecar it was read
  // from.
  struct EmbeddedEntry {
    qint64 modified = 0;
    qint64 bytes = 0;
    quint64 device = 0;
    quint64 inode = 0;
    QString sidecar;
    qint64 sidecarModified = 0;
    qint64 sidecarBytes = -1;
    EmbeddedMetadata::Fields fields;
    // When it was last read or matched, for the cache cap.
    qint64 seen = 0;
    // Loaded with its description left out to keep the cache under its cap:
    // its keywords seed the library, and the file is read again when next
    // seen so the description comes back.
    bool partial = false;
  };

  struct Entry {
    qint64 modified = 0;
    qint64 bytes = 0;
    Details details;
    quint64 device = 0;
    quint64 inode = 0;
  };

  void sync();
  void processNext();
  [[nodiscard]] bool startProbe();
  void processFinished(Probe* probe, bool successful);
  void finishProbe(Probe* probe, const Parsed& parsed);
  void cancelStaleProbes();
  [[nodiscard]] int inFlightCount() const;
  [[nodiscard]] bool isCurrent(const Candidate& candidate) const;
  [[nodiscard]] bool stillCurrent(const Candidate& candidate) const;
  [[nodiscard]] static QHash<int, Details> parseImageBatch(const QByteArray& output,
                                                           QSet<int>* completed);
  void flushUpdates();
  void adopt(const Candidate& candidate, const Details& details);
  void adoptEmbedded(const Candidate& candidate, const EmbeddedMetadata::Fields& fields);
  void flushEmbedded();
  // Hands cached values for files under the library's roots to AppSettings
  // before the first scan lands, so a saved view on a keyword from a file is
  // not cleared at launch.
  void seedEmbedded();
  [[nodiscard]] bool readsEmbedded() const;
  [[nodiscard]] bool embeddedMatches(const EmbeddedEntry& entry, const CaptureRecord& record) const;
  [[nodiscard]] bool embeddedStillCurrent(const Candidate& candidate) const;
  [[nodiscard]] static CaptureModel::MetadataUpdate updateFor(const Candidate& candidate,
                                                              const Details& details);
  void setIndexing(bool value);
  void resetProgress(int total);
  void advanceProgress(int amount = 1);
  void publishProgress();
  void loadCache();
  void loadEmbeddedCache();
  void scheduleSave();
  void saveCache();
  void saveEmbeddedCache();

  CaptureModel* m_model = nullptr;
  QString m_imageProgram;
  QString m_videoProgram;
  QHash<QString, Entry> m_entries;
  QList<Candidate> m_queue;
  QHash<QString, CaptureModel::MetadataUpdate> m_pendingUpdates;
  QHash<QString, QString> m_failed;
  QHash<QString, EmbeddedEntry> m_embedded;
  QHash<QString, EmbeddedEntry> m_pendingEmbedded;
  QTimer m_embeddedTimer;
  bool m_embeddedDirty = false;
  // Parsing and raw header reads run here, never on the window thread: a
  // slow disk or network share must not stall the window.
  QThreadPool m_pool;
  std::vector<std::unique_ptr<Probe>> m_probes;
  int m_parallel = 2;
  QTimer m_syncTimer;
  QTimer m_progressTimer;
  bool m_progressPending = false;
  QTimer m_saveTimer;
  QTimer m_updateTimer;
  std::shared_ptr<std::atomic_bool> m_stopping = std::make_shared<std::atomic_bool>(false);
  bool m_indexing = false;
  bool m_cacheDirty = false;
  int m_completed = 0;
  int m_total = 0;
};
