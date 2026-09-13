#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

// Sidecar subtitles: the .srt or .vtt sitting next to a video, which Qt
// Multimedia will not load by itself.
//
// The file is parsed once into sorted cues and cached against its size and
// mtime. textAt() is a binary search, so it is cheap enough to call on every
// playback tick from a QML binding. Nothing here writes or moves a file.
class SubtitleIndex final : public QObject {
  Q_OBJECT

public:
  struct Cue {
    qint64 start = 0;
    qint64 end = 0;
    QString text;
  };

  // Refuse absurd files rather than read a mislabelled video as text.
  static constexpr qint64 kMaximumFileBytes = 8 * 1024 * 1024;

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

private:
  struct Parsed {
    qint64 modified = -1;
    qint64 bytes = -1;
    QList<Cue> cues;
    bool valid = false;
  };

  [[nodiscard]] const QList<Cue>& cuesFor(const QString& path);

  QHash<QString, Parsed> m_cache;
};
