#pragma once

#include <QHash>
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

class QProcess;

// Sidecar subtitles: the .srt or .vtt sitting next to a video, which Qt
// Multimedia will not load by itself.
//
// Files are parsed on a worker and invalidated by filesystem watches. An
// interval index prunes expired cues on each playback tick, including overlaps.
// Nothing here writes or moves a file.
class SubtitleIndex final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
  struct Cue {
    qint64 start = 0;
    qint64 end = 0;
    QString text;
  };

  // Refuse absurd files rather than read a mislabelled video as text.
  static constexpr qint64 kMaximumFileBytes = 8 * 1024 * 1024;

#ifdef OMAROLL_FUZZ
  static void fuzzParse(const QString& path) { (void)parse(path); }
#endif

  explicit SubtitleIndex(QObject* parent = nullptr);

  // Sidecar files beside the video, labelled by their language tag when the
  // name carries one ("movie.en.srt" -> "EN"). Sorted and de-duplicated.
  Q_INVOKABLE [[nodiscard]] QStringList files(const QString& videoPath);

  // A short human label for a file from files().
  Q_INVOKABLE [[nodiscard]] QString label(const QString& subtitlePath) const;

  // The cue text at a playback position, or empty. Multiple overlapping cues
  // are joined with a newline. Options and cue spacing are ignored.
  Q_INVOKABLE [[nodiscard]] QString textAt(const QString& subtitlePath, qint64 positionMs);

  // Number of parsed cues, for tests and diagnostics.
  Q_INVOKABLE [[nodiscard]] int cueCount(const QString& subtitlePath);

  // An embedded text track (the video's track-th subtitle stream) copied out
  // with ffmpeg into a private SubRip file, so its cues can be shifted like a
  // sidecar's. Empty until the copy is ready; the first call starts it and
  // changed() follows. A copy lives until releaseEmbedded() or the app quits.
  Q_INVOKABLE [[nodiscard]] QString embedded(const QString& videoPath, int track);
  // For a track embedded() was asked for: "working", "ready", "failed" (a
  // picture-based track, which has no text to copy), "unavailable" (no
  // ffmpeg) or "slow" (ffmpeg read nothing for embeddedStallMs). Empty
  // before. embedded() never starts a slow copy again; retryEmbedded() does.
  Q_INVOKABLE [[nodiscard]] QString embeddedState(const QString& videoPath, int track) const;
  // Why a delay can't be applied to that track, in words, or empty.
  Q_INVOKABLE [[nodiscard]] QString embeddedProblem(const QString& videoPath, int track) const;
  // Starts a slow copy again. For an explicit request (Z, X), never a binding.
  Q_INVOKABLE void retryEmbedded(const QString& videoPath, int track);
  // Stops a running copy for the video and deletes its copies. Call when
  // the video changes or closes.
  Q_INVOKABLE void releaseEmbedded(const QString& videoPath);
  // Stops every copy. Runs on aboutToQuit, so quitting never waits on one.
  void stopEmbedded();
  // How long ffmpeg may read nothing before a copy gives up; a long file
  // that keeps reading has no limit. Tests shorten it.
  void setEmbeddedStallMs(int milliseconds) { m_embeddedStallMs = milliseconds; }
  static constexpr int kEmbeddedStallMs = 30000;

  ~SubtitleIndex() override;

  [[nodiscard]] int revision() const { return m_revision; }

signals:
  void changed();

private:
  struct Parsed {
    QList<Cue> cues;
    QList<qint64> maximumEnds;
    int leaves = 1;
  };

  [[nodiscard]] const Parsed& cuesFor(const QString& path);
  static Parsed parse(const QString& path);
  static void collect(const Parsed& parsed, int node, int begin, int end,
                      qint64 position, QStringList& active);
  void invalidate(const QString& path);

  struct Extraction {
    QString video;
    QString version;
    QString state;
    QString path;
    QPointer<QProcess> process;
  };
  void startEmbedded(const QString& key, const QString& videoPath, int track, const QString& version);
  void dropEmbedded(QHash<QString, Extraction>::iterator found);
  void finishEmbedded(const QString& key, QProcess* process, const QString& state);
  void notifyLater();
  static QString embeddedKey(const QString& videoPath, int track) {
    return videoPath + QLatin1Char('\n') + QString::number(track);
  }

  QHash<QString, Parsed> m_cache;
  QHash<QString, Extraction> m_extractions;
  // Made on first use, removed when the app quits.
  std::unique_ptr<QTemporaryDir> m_extracted;
  int m_extractedCount = 0;
  int m_embeddedStallMs = kEmbeddedStallMs;
  QHash<QString, quint64> m_pending;
  QFileSystemWatcher m_watcher;
  quint64 m_generation = 0;
  int m_revision = 0;
};
