#include "sources/CaptureScanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <sys/stat.h>

namespace {

const QStringList kImageSuffixes = {
    QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
    QStringLiteral("webp"), QStringLiteral("gif"),  QStringLiteral("bmp"),
    QStringLiteral("avif"), QStringLiteral("heic"), QStringLiteral("heif"),
    QStringLiteral("tiff"), QStringLiteral("tif"),
};

const QStringList kVideoSuffixes = {
    QStringLiteral("mp4"), QStringLiteral("mkv"),  QStringLiteral("webm"),
    QStringLiteral("mov"), QStringLiteral("avi"),  QStringLiteral("m4v"),
    QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("wmv"),
    QStringLiteral("flv"), QStringLiteral("ogv"),  QStringLiteral("3gp"),
    QStringLiteral("mts"), QStringLiteral("m2ts"),
};

// Producers Omarchy users actually have installed. Each entry pairs a name
// pattern with the kind it implies; capture group 1, when present, is a
// timestamp parsed with the matching format.
struct NamePattern {
  QRegularExpression expression;
  CaptureRecord::Kind kind;
  QString timestampFormat;
};

const QList<NamePattern>& namePatterns() {
  static const QList<NamePattern> patterns = {
      // Omarchy's own, the two that matter most.
      // screenshot-2026-08-31_23-26-39.png
      // Derived files keep the kind and the moment of their source, so a
      // -matte.png, a -720p.gif or a -1080p.mp4 sits beside what it came from.
      {QRegularExpression(
           QStringLiteral(R"(^screenshot-(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})(?:-[\w-]+)?\.)"),
           QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Screenshot, QStringLiteral("yyyy-MM-dd_HH-mm-ss")},
      // screenrecording-2026-08-31_23-26-39.mp4
      {QRegularExpression(
           QStringLiteral(R"(^screenrecording-(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})(?:-[\w-]+)?\.)"),
           QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Recording, QStringLiteral("yyyy-MM-dd_HH-mm-ss")},

      // grim's conventional output: 20260831_23h26m39s_grim.png
      {QRegularExpression(QStringLiteral(R"(^(\d{8}_\d{2}h\d{2}m\d{2}s)_grim\.)"),
                          QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Screenshot, QStringLiteral("yyyyMMdd_HH'h'mm'm'ss's'")},

      // Flameshot and the GNOME-style label a lot of tools copy.
      {QRegularExpression(QStringLiteral(R"(^(?:flameshot|screenshot[ _]from)[ _-])"),
                          QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Screenshot, {}},

      // OBS default: 2026-08-31 23-26-39.mkv
      {QRegularExpression(QStringLiteral(R"(^(\d{4}-\d{2}-\d{2} \d{2}-\d{2}-\d{2})\.)")),
       CaptureRecord::Recording, QStringLiteral("yyyy-MM-dd HH-mm-ss")},

      // gpu-screen-recorder when driven outside Omarchy's wrapper.
      {QRegularExpression(QStringLiteral(R"(^(?:video|replay)[ _-]\d)"),
                          QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Recording, {}},

      // Bare "Screenshot ..." from assorted tools, kept last so the precise
      // patterns above win.
      {QRegularExpression(QStringLiteral(R"(^screenshot)"),
                          QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Screenshot, {}},
  };
  return patterns;
}

// Directories that are never content: caches, thumbnails, version control, and
// omaroll's own output so a matte composite is not read back as a new capture
// that then gets a matte of its own.
bool isSkippedDirectory(const QString& name) {
  if (name.startsWith(QLatin1Char('.'))) {
    return true;
  }
  static const QSet<QString> skipped = {
      QStringLiteral("omaroll"),
      QStringLiteral("thumbnails"),
      QStringLiteral("Thumbnails"),
  };
  return skipped.contains(name);
}

// omarchy-capture-screenrecording writes screenrecording-<stamp>-preview.png
// for its notification and deletes it two seconds later, and stages
// screenrecording-<stamp>-processed.mp4 before moving it over the original.
// Neither is a capture and both would flash into the grid otherwise.
bool isTransientCaptureArtifact(const QString& name) {
  static const QRegularExpression transient(
      QStringLiteral(R"(^screenrecording-\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}-(?:preview\.png|processed\.mp4)$)"),
      QRegularExpression::CaseInsensitiveOption);
  return transient.match(name).hasMatch();
}

} // namespace

bool CaptureScanner::isImage(const QString& suffix) {
  return kImageSuffixes.contains(suffix.toLower());
}

bool CaptureScanner::isVideo(const QString& suffix) {
  return kVideoSuffixes.contains(suffix.toLower());
}

bool CaptureScanner::classifyByName(const QString& fileName, CaptureRecord::Kind& kind,
                                    QDateTime& captured) {
  for (const NamePattern& pattern : namePatterns()) {
    const QRegularExpressionMatch match = pattern.expression.match(fileName);
    if (!match.hasMatch()) {
      continue;
    }

    kind = pattern.kind;

    if (!pattern.timestampFormat.isEmpty() && match.lastCapturedIndex() >= 1) {
      const QDateTime stamped =
          QDateTime::fromString(match.captured(1), pattern.timestampFormat);
      if (stamped.isValid()) {
        captured = stamped;
      }
    }
    return true;
  }
  return false;
}

QList<CaptureRecord> CaptureScanner::scan(const QList<Root>& roots,
                                          const std::atomic_bool* cancel,
                                          QStringList* traversedDirectories) {
  QList<CaptureRecord> records;
  if (traversedDirectories) {
    traversedDirectories->clear();
  }

  // Canonical paths stop the same file arriving twice through a symlinked
  // directory, or through two roots that resolve to the same place.
  QSet<QString> seenFiles;
  QSet<QString> reportedDirectories;

  for (const Root& root : roots) {
    const QString canonicalRoot = QFileInfo(root.path).canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
      continue;
    }

    // Per root, not shared: the loop guard only has to stop a symlink cycle
    // inside one walk. Shared across roots it would let a shallow root claim a
    // directory and silently stop a deeper root from ever descending into it,
    // which is exactly the default layout, where the depth-1 screenshot root
    // and the recursive Pictures root are the same directory.
    QSet<QString> seenDirectories;

    // Breadth-first by depth so maxDepth is a real bound rather than a guess
    // about QDirIterator's traversal order.
    QList<QPair<QString, int>> pending{{canonicalRoot, 1}};

    while (!pending.isEmpty()) {
      if (cancel && cancel->load()) {
        return records;
      }
      const auto [directory, depth] = pending.takeFirst();
      if (seenDirectories.contains(directory)) {
        continue;
      }
      seenDirectories.insert(directory);
      if (traversedDirectories && !reportedDirectories.contains(directory)) {
        reportedDirectories.insert(directory);
        traversedDirectories->append(directory);
      }

      QDir dir(directory);
      const QFileInfoList entries =
          dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);

      for (const QFileInfo& entry : entries) {
        const QString name = entry.fileName();

        if (entry.isDir()) {
          if (depth < root.maxDepth && !isSkippedDirectory(name)) {
            const QString canonicalChild = entry.canonicalFilePath();
            if (!canonicalChild.isEmpty()) {
              pending.append({canonicalChild, depth + 1});
            }
          }
          continue;
        }

        if (name.startsWith(QLatin1Char('.')) || isTransientCaptureArtifact(name)) {
          continue;
        }

        const QString suffix = entry.suffix();
        const bool image = isImage(suffix);
        const bool video = isVideo(suffix);
        if (!image && !video) {
          continue;
        }

        // The directory is already canonical, so only a symlink needs the
        // realpath walk; on a slow mount that is one syscall chain per file.
        const QString canonicalFile = entry.isSymLink()
                                          ? entry.canonicalFilePath()
                                          : directory + QLatin1Char('/') + name;
        if (canonicalFile.isEmpty() || seenFiles.contains(canonicalFile)) {
          continue;
        }
        seenFiles.insert(canonicalFile);

        CaptureRecord record;
        record.path = canonicalFile;
        record.fileName = name;
        record.bytes = entry.size();
        record.modified = entry.lastModified().toMSecsSinceEpoch();
        record.video = video;
        record.captured = entry.lastModified();
        struct stat status {};
        if (::stat(QFile::encodeName(canonicalFile).constData(), &status) == 0) {
          record.device = status.st_dev;
          record.inode = status.st_ino;
        }

        CaptureRecord::Kind named = CaptureRecord::Picture;
        QDateTime namedTime = record.captured;
        if (classifyByName(name, named, namedTime)) {
          record.kind = named;
          record.captured = namedTime;

          // A name that says "screenshot" on a video file is still a video.
          // Trust the medium over the label when the two disagree.
          if (video && record.kind == CaptureRecord::Screenshot) {
            record.kind = CaptureRecord::Recording;
          } else if (image && record.kind == CaptureRecord::Recording) {
            record.kind = CaptureRecord::Screenshot;
          }
        } else {
          // No producer signature, so the root decides, by medium.
          record.kind = video ? root.videoFallback : root.imageFallback;
        }

        records.append(record);
      }
    }
  }

  std::sort(records.begin(), records.end(),
            [](const CaptureRecord& first, const CaptureRecord& second) {
              if (first.captured != second.captured) {
                return first.captured > second.captured;
              }
              return first.path < second.path;
            });

  return records;
}
