#pragma once

#include "library/CaptureModel.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

// Reads embedded metadata from general photos and videos: the original
// capture date, and the camera and lens that took them.
//
// Discovery itself stays a fast filesystem walk. Once a scan lands, this
// class checks small image batches with ImageMagick and videos one at a time
// with ffprobe, then replaces only mtime fallbacks. Omarchy's timestamped
// capture names always remain authoritative. Results, including "nothing
// embedded", are cached by path and filesystem identity so an unchanged
// library is not probed every launch.
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

  [[nodiscard]] bool indexing() const { return m_indexing; }
  [[nodiscard]] int completed() const { return m_completed; }
  [[nodiscard]] int total() const { return m_total; }

  [[nodiscard]] static QString cachePath();
  [[nodiscard]] static QDateTime parseImageDate(const QByteArray& output);
  [[nodiscard]] static Details parseImageDetails(const QByteArray& output);
  [[nodiscard]] static QDateTime parseVideoDate(const QByteArray& output);
  [[nodiscard]] static Details parseVideoDetails(const QByteArray& output);
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
  void finishCurrent(bool successful);
  [[nodiscard]] bool isCurrent(const Candidate& candidate) const;
  [[nodiscard]] bool stillCurrent(const Candidate& candidate) const;
  [[nodiscard]] static QHash<int, Details> parseImageBatch(const QByteArray& output,
                                                           QSet<int>* completed);
  void adopt(const Candidate& candidate, const Details& details);
  [[nodiscard]] static CaptureModel::MetadataUpdate updateFor(const Candidate& candidate,
                                                              const Details& details);
  void setIndexing(bool value);
  void resetProgress(int total);
  void advanceProgress(int amount = 1);
  void loadCache();
  void scheduleSave();
  void saveCache();

  CaptureModel* m_model = nullptr;
  QString m_imageProgram;
  QString m_videoProgram;
  QHash<QString, Entry> m_entries;
  QList<Candidate> m_queue;
  QHash<QString, CaptureModel::MetadataUpdate> m_pendingUpdates;
  QHash<QString, QString> m_failed;
  QList<Candidate> m_current;
  QProcess m_process;
  QTimer m_timeout;
  QTimer m_syncTimer;
  QTimer m_saveTimer;
  bool m_indexing = false;
  bool m_cacheDirty = false;
  int m_completed = 0;
  int m_total = 0;
};
