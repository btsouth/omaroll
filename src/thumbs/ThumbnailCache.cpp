#include "thumbs/ThumbnailCache.h"

#include "sources/CaptureScanner.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>

namespace {

// Files that produced no thumbnail, remembered for a while so a recording
// still being written, or a corrupt download in view, does not cost an
// ffmpegthumbnailer timeout every time its delegate is recycled. Keyed on the
// same identity as the disk cache, so a rewrite is tried again at once.
constexpr qint64 kNegativeCacheMs = 60 * 1000;
QMutex g_negativeMutex;
QHash<QString, qint64> g_negativeUntil;

bool recentlyFailed(const QString& key) {
  QMutexLocker lock(&g_negativeMutex);
  const auto it = g_negativeUntil.constFind(key);
  if (it == g_negativeUntil.constEnd()) {
    return false;
  }
  if (it.value() < QDateTime::currentMSecsSinceEpoch()) {
    g_negativeUntil.erase(it);
    return false;
  }
  return true;
}

void rememberFailure(const QString& key) {
  QMutexLocker lock(&g_negativeMutex);
  g_negativeUntil.insert(key, QDateTime::currentMSecsSinceEpoch() + kNegativeCacheMs);
}

// Written as JPEG: these are opaque photographic tiles, and JPEG at 88 keeps a
// large library's cache a fraction of the PNG equivalent.
constexpr int kJpegQuality = 88;

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured;
}

} // namespace

QString ThumbnailCache::cacheDirectory() {
  return cacheHome() + QStringLiteral("/omaroll/thumbs");
}

void ThumbnailCache::prune(qint64 maxBytes) {
  QDir dir(cacheDirectory());
  if (!dir.exists()) {
    return;
  }

  // Oldest first, by last access where the filesystem tracks it and last
  // modification otherwise, so a tile that is still being scrolled past
  // survives ahead of one nobody has looked at in months.
  QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.jpg")}, QDir::Files);

  qint64 total = 0;
  for (const QFileInfo& entry : entries) {
    total += entry.size();
  }
  if (total <= maxBytes) {
    return;
  }

  std::sort(entries.begin(), entries.end(), [](const QFileInfo& a, const QFileInfo& b) {
    const QDateTime left = a.lastRead().isValid() ? a.lastRead() : a.lastModified();
    const QDateTime right = b.lastRead().isValid() ? b.lastRead() : b.lastModified();
    return left < right;
  });

  for (const QFileInfo& entry : entries) {
    if (total <= maxBytes) {
      break;
    }
    const qint64 size = entry.size();
    if (QFile::remove(entry.absoluteFilePath())) {
      total -= size;
    }
  }
}

QString ThumbnailCache::cacheKey(const QString& path, const QSize& pixelSize, int seekPercent) {
  const QFileInfo info(path);

  // Same identity Omarchy's own image picker uses (size and mtime), plus the
  // rendered size so a scale change is a miss rather than a blurry hit. The
  // leading tag versions the rendering itself, so a change in how tiles are
  // made invalidates everything rather than serving old shapes from cache.
  const QString identity = QStringLiteral("cover1|%1|%2|%3|%4x%5|t%6")
                               .arg(path)
                               .arg(info.size())
                               .arg(info.lastModified().toMSecsSinceEpoch())
                               .arg(pixelSize.width())
                               .arg(pixelSize.height())
                               .arg(seekPercent);

  return QString::fromLatin1(
      QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Md5).toHex());
}

QImage ThumbnailCache::renderImage(const QString& path, const QSize& pixelSize) {
  QImageReader reader(path);
  reader.setAutoTransform(true);

  const QSize original = reader.size();
  if (original.isValid() && !original.isEmpty()) {
    // Scaled decode. For JPEG this hands libjpeg a scale denominator so a
    // 12-megapixel photo is never fully decoded to draw a 300px tile.
    QSize target = original.scaled(pixelSize, Qt::KeepAspectRatioByExpanding);
    if (target.isEmpty()) {
      target = pixelSize;
    }
    // Never upscale: a small source stays its own size rather than being
    // stretched into a soft tile.
    if (target.width() > original.width() || target.height() > original.height()) {
      target = original;
    }
    reader.setScaledSize(target);
  }

  return reader.read();
}

QImage ThumbnailCache::renderVideo(const QString& path, const QSize& pixelSize, int seekPercent) {
  const QString executable = QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer"));
  if (executable.isEmpty()) {
    return {};
  }

  QTemporaryDir temporary;
  if (!temporary.isValid()) {
    return {};
  }
  const QString output = temporary.filePath(QStringLiteral("frame.png"));

  // ffmpegthumbnailer sizes by longest edge and the frame's aspect is unknown
  // until it is decoded, so ask for twice the tile's long edge: enough for
  // anything from portrait to ultrawide to cover the tile after the cover
  // scale below. Capped so a large detail stage never asks for an upscale of
  // a 1080p source.
  const int longest = std::min(2 * std::max(pixelSize.width(), pixelSize.height()), 1920);

  QProcess process;
  process.start(executable, {
                                QStringLiteral("-i"), path,
                                QStringLiteral("-o"), output,
                                QStringLiteral("-s"), QString::number(longest),
                                QStringLiteral("-t"),
                                QStringLiteral("%1%").arg(qBound(0, seekPercent, 95)),
                                QStringLiteral("-q"), QStringLiteral("8"),
                            });

  if (!process.waitForFinished(8000) || process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 0) {
    return {};
  }

  return QImage(output);
}

QImage ThumbnailCache::thumbnail(const QString& path, const QSize& logicalSize,
                                 qreal devicePixelRatio, int seekPercent) {
  if (path.isEmpty() || !QFileInfo::exists(path)) {
    return {};
  }

  const qreal ratio = std::clamp(devicePixelRatio, qreal(1.0), kMaxDevicePixelRatio);
  const QSize pixelSize(static_cast<int>(std::ceil(logicalSize.width() * ratio)),
                        static_cast<int>(std::ceil(logicalSize.height() * ratio)));
  if (pixelSize.isEmpty()) {
    return {};
  }

  const QString directory = cacheDirectory();
  const QString key = cacheKey(path, pixelSize, seekPercent);
  const QString cachePath = directory + QLatin1Char('/') + key + QStringLiteral(".jpg");

  QImage cached(cachePath);
  if (!cached.isNull()) {
    return cached;
  }
  if (recentlyFailed(key)) {
    return {};
  }

  const QString suffix = QFileInfo(path).suffix();
  QImage rendered = CaptureScanner::isVideo(suffix) ? renderVideo(path, pixelSize, seekPercent)
                                                    : renderImage(path, pixelSize);
  if (rendered.isNull() && !CaptureScanner::isVideo(suffix)) {
    // Video bytes under an image name happen; ffmpegthumbnailer can read it.
    rendered = renderVideo(path, pixelSize, seekPercent);
  }
  if (rendered.isNull()) {
    rememberFailure(key);
    return {};
  }

  // Scale down until the short side meets the tile. Only when both sides
  // overhang: otherwise expanding to cover would be an upscale.
  if (rendered.width() > pixelSize.width() && rendered.height() > pixelSize.height()) {
    rendered = rendered.scaled(pixelSize, Qt::KeepAspectRatioByExpanding,
                               Qt::SmoothTransformation);
  }

  // A panorama covered on its short side is enormous on its long one; the
  // grid crops to the tile anyway, so keep at most twice the tile around the
  // centre. Past that the texture is wasted memory, and past the driver's
  // limit it would not draw at all.
  const int maxWidth = pixelSize.width() * 2;
  const int maxHeight = pixelSize.height() * 2;
  if (rendered.width() > maxWidth || rendered.height() > maxHeight) {
    const int width = std::min(rendered.width(), maxWidth);
    const int height = std::min(rendered.height(), maxHeight);
    rendered = rendered.copy((rendered.width() - width) / 2, (rendered.height() - height) / 2,
                             width, height);
  }

  // JPEG has no alpha and an odd source format (CMYK, 16-bit) is best settled
  // here rather than at texture upload. Transparent pixels land on a neutral
  // dark ground instead of whatever the raw channels held.
  if (rendered.hasAlphaChannel()) {
    QImage flat(rendered.size(), QImage::Format_RGB32);
    flat.fill(QColor(30, 30, 30));
    QPainter painter(&flat);
    painter.drawImage(0, 0, rendered);
    painter.end();
    rendered = flat;
  } else if (rendered.format() != QImage::Format_RGB32) {
    rendered = rendered.convertToFormat(QImage::Format_RGB32);
  }

  // Best effort. A cache that cannot be written still returns a correct image;
  // it just costs the decode again next time. Written beside and renamed into
  // place, so a second worker asking for the same tile mid-write reads either
  // nothing or a whole file, never a torn one.
  QDir().mkpath(directory);
  const QString staging = QStringLiteral("%1.%2.%3.tmp")
                              .arg(cachePath)
                              .arg(QCoreApplication::applicationPid())
                              .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
  if (rendered.save(staging, "JPG", kJpegQuality) && !QFile::rename(staging, cachePath)) {
    QFile::remove(staging);
  }

  return rendered;
}
