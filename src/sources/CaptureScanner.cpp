#include "sources/CaptureScanner.h"
#include "sources/ThumbnailSource.h"

#include "sources/CameraRaw.h"
#include "sources/RawJpegPairs.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

#include <sys/stat.h>

namespace {

const QStringList kImageSuffixes = {
    QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("webp"),
    QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("avif"), QStringLiteral("heic"),
    QStringLiteral("heif"), QStringLiteral("tiff"), QStringLiteral("tif"),  QStringLiteral("svg"),
    QStringLiteral("svgz"), QStringLiteral("ico"),  QStringLiteral("jxl"),  QStringLiteral("jp2"),
    QStringLiteral("j2k"),  QStringLiteral("qoi"),  QStringLiteral("psd"),  QStringLiteral("dds"),
    QStringLiteral("exr"),  QStringLiteral("tga"),
};

const QStringList kVideoSuffixes = {
    QStringLiteral("mp4"), QStringLiteral("mkv"),  QStringLiteral("webm"), QStringLiteral("mov"),
    QStringLiteral("avi"), QStringLiteral("m4v"),  QStringLiteral("mpg"),  QStringLiteral("mpeg"),
    QStringLiteral("wmv"), QStringLiteral("flv"),  QStringLiteral("ogv"),  QStringLiteral("3gp"),
    QStringLiteral("mts"), QStringLiteral("m2ts"), QStringLiteral("m2t"),  QStringLiteral("3gpp"),
    QStringLiteral("3g2"), QStringLiteral("asf"),  QStringLiteral("ogm"),  QStringLiteral("divx"),
    QStringLiteral("f4v"), QStringLiteral("qt"),   QStringLiteral("vob"),  QStringLiteral("mpe"),
};

const QStringList kDocumentSuffixes = {QStringLiteral("pdf")};

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
           QStringLiteral(
               R"(^screenrecording-(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})(?:-[\w-]+)?\.)"),
           QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Recording, QStringLiteral("yyyy-MM-dd_HH-mm-ss")},

      // grim's conventional output: 20260831_23h26m39s_grim.png
      {QRegularExpression(QStringLiteral(R"(^(\d{8}_\d{2}h\d{2}m\d{2}s)_grim\.)"),
                          QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Screenshot, QStringLiteral("yyyyMMdd_HH'h'mm'm'ss's'")},

      // Flameshot and the GNOME-style label a lot of tools copy.
      {QRegularExpression(QStringLiteral(R"(^(?:flameshot|screenshot[ _]from)[ _-])"),
                          QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Screenshot,
       {}},

      // OBS default: 2026-08-31 23-26-39.mkv
      {QRegularExpression(QStringLiteral(R"(^(\d{4}-\d{2}-\d{2} \d{2}-\d{2}-\d{2})\.)")),
       CaptureRecord::Recording, QStringLiteral("yyyy-MM-dd HH-mm-ss")},

      // gpu-screen-recorder when driven outside Omarchy's wrapper.
      {QRegularExpression(QStringLiteral(R"(^(?:video|replay)[ _-]\d)"),
                          QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Recording,
       {}},

      // Bare "Screenshot ..." from assorted tools, kept last so the precise
      // patterns above win.
      {QRegularExpression(QStringLiteral(R"(^screenshot)"),
                          QRegularExpression::CaseInsensitiveOption),
       CaptureRecord::Screenshot,
       {}},
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
      QStringLiteral(
          R"(^screenrecording-\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}-(?:preview\.png|processed\.mp4)$)"),
      QRegularExpression::CaseInsensitiveOption);
  return transient.match(name).hasMatch();
}

} // namespace

bool CaptureScanner::isImage(const QString& suffix) {
  return kImageSuffixes.contains(suffix.toLower()) || CameraRaw::isRaw(suffix);
}

bool CaptureScanner::isVideo(const QString& suffix) {
  return kVideoSuffixes.contains(suffix.toLower());
}

bool CaptureScanner::isDocument(const QString& suffix) {
  return kDocumentSuffixes.contains(suffix.toLower());
}

bool CaptureScanner::isSupported(const QString& suffix) {
  return isImage(suffix) || isVideo(suffix) || isDocument(suffix);
}

namespace {

// One EBML variable-length number. Element IDs keep their length marker;
// sizes drop it, and a size of all ones means "unknown", which live
// recorders write for the segment.
bool readVint(const QByteArray& data, qsizetype& at, quint64& value, bool keepMarker,
              bool* unknown = nullptr) {
  if (at >= data.size()) {
    return false;
  }
  const auto first = static_cast<unsigned char>(data.at(at));
  int width = 1;
  while (width <= 8 && !(first & (0x80 >> (width - 1)))) {
    ++width;
  }
  if (width > 8 || at + width > data.size()) {
    return false;
  }
  const unsigned char payload = 0xFF >> width;
  quint64 result = keepMarker ? first : (first & payload);
  bool allOnes = (first & payload) == payload;
  for (int i = 1; i < width; ++i) {
    const auto byte = static_cast<unsigned char>(data.at(at + i));
    result = (result << 8) | byte;
    allOnes = allOnes && byte == 0xFF;
  }
  at += width;
  value = result;
  if (unknown) {
    *unknown = !keepMarker && allOnes;
  }
  return true;
}

constexpr quint64 kSegment = 0x18538067;
constexpr quint64 kTracks = 0x1654AE6B;
constexpr quint64 kTrackEntry = 0xAE;
constexpr quint64 kTrackType = 0x83;
constexpr quint64 kCodecId = 0x86;

// A track entry is video by its type (1) or a codec ID such as V_VP9.
bool isVideoTrack(const QByteArray& data, qsizetype at, qsizetype end) {
  while (at < end) {
    quint64 id = 0;
    quint64 size = 0;
    if (!readVint(data, at, id, true) || !readVint(data, at, size, false)) {
      return false;
    }
    const qsizetype length = qsizetype(std::min<quint64>(size, quint64(end - at)));
    if (id == kTrackType && length == 1) {
      return data.at(at) == 1;
    }
    if (id == kCodecId && data.mid(at, std::min<qsizetype>(length, 2)) == "V_") {
      return true;
    }
    at += length;
  }
  return false;
}

// Walks only Segment and Tracks on the way to the track entries, skipping
// everything else by its size, all within the header that was read.
bool hasVideoTrackIn(const QByteArray& data, qsizetype at, qsizetype end, int depth) {
  while (at < end) {
    quint64 id = 0;
    quint64 size = 0;
    bool unknown = false;
    if (!readVint(data, at, id, true) || !readVint(data, at, size, false, &unknown)) {
      return false;
    }
    const qsizetype stop = unknown || size > quint64(end - at) ? end : at + qsizetype(size);
    if (id == kTrackEntry && depth == 2) {
      if (isVideoTrack(data, at, stop)) {
        return true;
      }
    } else if ((id == kSegment && depth == 0) || (id == kTracks && depth == 1)) {
      if (hasVideoTrackIn(data, at, stop, depth + 1)) {
        return true;
      }
    } else if (unknown) {
      // Nothing past an element of unknown size can be found.
      return false;
    }
    at = stop;
  }
  return false;
}

bool hasMatroskaVideoTrack(const QByteArray& header) {
  return hasVideoTrackIn(header, 0, header.size(), 0);
}

// The medium of an extensionless file, from its first 16 KiB.
QString sniffedSuffix(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  // Explicit MIME names prevent generic binary, XML, audio or archive types
  // from inheriting a media extension through a broad glob or alias.
  static const QHash<QString, QString> formats = {
      {"image/png", "png"}, {"image/jpeg", "jpg"}, {"image/webp", "webp"},
      {"image/gif", "gif"}, {"image/bmp", "bmp"}, {"image/avif", "avif"},
      {"image/heic", "heic"}, {"image/heif", "heif"}, {"image/tiff", "tiff"},
      {"image/svg+xml", "svg"}, {"image/vnd.microsoft.icon", "ico"},
      {"image/jxl", "jxl"}, {"image/jp2", "jp2"}, {"image/x-jp2-codestream", "j2k"},
      {"image/qoi", "qoi"}, {"image/vnd.adobe.photoshop", "psd"},
      {"image/vnd.ms-dds", "dds"}, {"image/x-exr", "exr"},
      {"video/mp4", "mp4"}, {"video/x-m4v", "m4v"}, {"video/x-matroska", "mkv"},
      {"video/webm", "webm"}, {"video/quicktime", "mov"}, {"video/x-msvideo", "avi"},
      {"video/x-ms-wmv", "wmv"}, {"video/x-flv", "flv"}, {"video/mpeg", "mpeg"},
      {"video/ogg", "ogv"}, {"video/3gpp", "3gp"}, {"video/mp2t", "mts"},
      // Canonical names newer shared-mime-info reports for the aliases above.
      {"video/vnd.avi", "avi"}, {"video/x-theora+ogg", "ogv"}, {"video/3gpp2", "3gp"},
      {"video/x-ogm+ogg", "ogm"},
  };
  const QByteArray header = file.read(16 * 1024);
  const QString mime = QMimeDatabase().mimeTypeForData(header).name();
  // Newer shared-mime-info reports generic Matroska without saying whether it
  // holds video, so require a track whose CodecID is a video codec.
  if (mime == u"application/x-matroska") {
    return hasMatroskaVideoTrack(header) ? QStringLiteral("mkv") : QString();
  }
  return formats.value(mime);
}

} // namespace

QString CaptureScanner::mediaSuffix(const QString& path) {
  if (path.isEmpty()) {
    return {};
  }
  const QFileInfo info(path);
  const QString suffix = info.suffix().toLower();
  if (!suffix.isEmpty()) {
    return suffix;
  }
  if (!info.isFile() || !info.isReadable() || info.size() == 0) {
    return {};
  }
  // Rescans and viewer steps ask again for the same files; read each one
  // once until it changes.
  struct Known {
    qint64 modified = 0;
    qint64 bytes = 0;
    QString suffix;
  };
  static QMutex mutex;
  static QHash<QString, Known> known;
  const qint64 modified = info.lastModified().toMSecsSinceEpoch();
  const qint64 bytes = info.size();
  {
    const QMutexLocker lock(&mutex);
    const auto found = known.constFind(path);
    if (found != known.cend() && found->modified == modified && found->bytes == bytes) {
      return found->suffix;
    }
  }
  const QString sniffed = sniffedSuffix(path);
  const QMutexLocker lock(&mutex);
  if (known.size() >= 4096) {
    known.clear();
  }
  known.insert(path, {modified, bytes, sniffed});
  return sniffed;
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
      const QDateTime stamped = QDateTime::fromString(match.captured(1), pattern.timestampFormat);
      if (stamped.isValid()) {
        captured = stamped;
      }
    }
    return true;
  }
  return false;
}

QList<CaptureRecord> CaptureScanner::scan(const QList<Root>& roots, const std::atomic_bool* cancel,
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
    const QFileInfo rootInfo(root.path);
    const bool explicitFile = rootInfo.isFile();
    const QString canonicalRoot = explicitFile ? rootInfo.canonicalPath()
                                              : rootInfo.canonicalFilePath();
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
    QList<QPair<QString, int>> pending {{canonicalRoot, 1}};

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
      const QFileInfoList entries = explicitFile
          ? QFileInfoList{rootInfo}
          : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);

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

        if (!explicitFile && (name.startsWith(QLatin1Char('.')) || isTransientCaptureArtifact(name))) {
          continue;
        }

        const QString suffix = mediaSuffix(entry.absoluteFilePath());
        const bool image = isImage(suffix);
        const bool video = isVideo(suffix);
        const bool document = isDocument(suffix);
        if (!image && !video && !document) {
          continue;
        }

        // The directory is already canonical, so only a symlink needs the
        // realpath walk; on a slow mount that is one syscall chain per file.
        const QString canonicalFile =
            entry.isSymLink() ? entry.canonicalFilePath() : directory + QLatin1Char('/') + name;
        if (canonicalFile.isEmpty() || seenFiles.contains(canonicalFile)) {
          continue;
        }
        seenFiles.insert(canonicalFile);

        CaptureRecord record;
        record.path = canonicalFile;
        record.entryPath = entry.absoluteFilePath();
        record.fileName = name;
        record.bytes = entry.size();
        record.modified = entry.lastModified().toMSecsSinceEpoch();
        record.video = video;
        record.document = document;
        record.animated = image && (suffix == u"gif" || suffix == u"webp");
        record.raw = image && CameraRaw::isRaw(suffix);
        record.captured = entry.lastModified();
        struct stat status {};
        if (::stat(QFile::encodeName(canonicalFile).constData(), &status) == 0) {
          record.device = status.st_dev;
          record.inode = status.st_ino;
          record.thumbnailVersion = ThumbnailSource::version(canonicalFile, FileVersion::key(status));
        }

        CaptureRecord::Kind named = CaptureRecord::Picture;
        QDateTime namedTime;
        if (document) {
          record.kind = root.imageFallback == CaptureRecord::Download
                            ? CaptureRecord::Download
                            : CaptureRecord::Document;
        } else if (classifyByName(name, named, namedTime)) {
          record.kind = named;
          if (namedTime.isValid()) {
            record.captured = namedTime;
            record.hasProducerTimestamp = true;
          }

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

  RawJpegPairs::assign(records);
  return records;
}
