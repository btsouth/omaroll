#include "thumbs/ThumbnailCache.h"

#include "sources/CaptureScanner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>

namespace {

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

QString ThumbnailCache::cacheKey(const QString& path, const QSize& pixelSize, int seekPercent) {
  const QFileInfo info(path);

  // Same identity Omarchy's own image picker uses (size and mtime), plus the
  // rendered size so a scale change is a miss rather than a blurry hit.
  const QString identity = QStringLiteral("%1|%2|%3|%4x%5|t%6")
                               .arg(path)
                               .arg(info.size())
                               .arg(info.lastModified().toSecsSinceEpoch())
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
    QSize target = original.scaled(pixelSize, Qt::KeepAspectRatio);
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

  QProcess process;
  process.start(executable, {
                                QStringLiteral("-i"), path,
                                QStringLiteral("-o"), output,
                                // Longest edge; ffmpegthumbnailer preserves aspect.
                                QStringLiteral("-s"),
                                QString::number(std::max(pixelSize.width(), pixelSize.height())),
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
  const QString cachePath =
      directory + QLatin1Char('/') + cacheKey(path, pixelSize, seekPercent) + QStringLiteral(".jpg");

  QImage cached(cachePath);
  if (!cached.isNull()) {
    return cached;
  }

  const QString suffix = QFileInfo(path).suffix();
  QImage rendered = CaptureScanner::isVideo(suffix) ? renderVideo(path, pixelSize, seekPercent)
                                                    : renderImage(path, pixelSize);
  if (rendered.isNull()) {
    return {};
  }

  if (rendered.size().width() > pixelSize.width() ||
      rendered.size().height() > pixelSize.height()) {
    rendered = rendered.scaled(pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }

  // Best effort. A cache that cannot be written still returns a correct image;
  // it just costs the decode again next time.
  QDir().mkpath(directory);
  rendered.save(cachePath, "JPG", kJpegQuality);

  return rendered;
}
