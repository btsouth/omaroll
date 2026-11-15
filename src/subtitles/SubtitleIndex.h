#pragma once

#include <QHash>
#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QThreadPool>

#include <atomic>
#include <memory>
#include <QString>
#include <QStringList>

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
  ~SubtitleIndex() override;

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

  // A text subtitle track inside a video, read once with ffmpeg on a worker.
  // Qt's player draws an embedded cue only while it plays, so a paused video
  // that has its subtitles switched on, or is sought, shows its cue from
  // here. Picture subtitles and a missing ffmpeg give no cues, and
  // embeddedReady() stays false so the player's own text is kept.
  Q_INVOKABLE [[nodiscard]] bool embeddedReady(const QString& videoPath, int track);
  Q_INVOKABLE [[nodiscard]] QString embeddedTextAt(const QString& videoPath, int track,
                                                   qint64 positionMs);

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
  static Parsed parseText(QString text, bool webvtt);
  static Parsed extract(const QString& videoPath, int track, const std::atomic_bool& stop);
  [[nodiscard]] const Parsed* embeddedFor(const QString& videoPath, int track);
  static void collect(const Parsed& parsed, int node, int begin, int end,
                      qint64 position, QStringList& active);
  void invalidate(const QString& path);

  QHash<QString, Parsed> m_cache;
  QHash<QString, quint64> m_pending;
  QHash<QString, Parsed> m_embedded;
  QSet<QString> m_embeddedPending;
  // Reading a track waits on ffmpeg; it gets its own thread rather than one
  // the rest of the app shares.
  QThreadPool m_extractors;
  // Set as the index goes, so a read under way stops its ffmpeg at once.
  std::shared_ptr<std::atomic_bool> m_stop = std::make_shared<std::atomic_bool>(false);
  QFileSystemWatcher m_watcher;
  quint64 m_generation = 0;
  int m_revision = 0;
};
