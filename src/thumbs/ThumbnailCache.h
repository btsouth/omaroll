#pragma once

#include <QImage>
#include <QSize>
#include <QString>

// Produces and caches thumbnails on disk.
//
// The cache key includes the rendered pixel size, not just the file identity,
// so moving the window to a monitor with a different scale factor regenerates
// rather than upscaling a thumbnail that was correct on the old one.
class ThumbnailCache {
public:
  // Hard ceiling on device pixel ratio. Past 2x the extra pixels cost decode
  // time and cache space without being visible.
  static constexpr qreal kMaxDevicePixelRatio = 2.0;

  [[nodiscard]] static QImage thumbnail(const QString& path, const QSize& logicalSize,
                                        qreal devicePixelRatio);

  [[nodiscard]] static QString cacheDirectory();

private:
  [[nodiscard]] static QImage renderImage(const QString& path, const QSize& pixelSize);
  [[nodiscard]] static QImage renderVideo(const QString& path, const QSize& pixelSize);
  [[nodiscard]] static QString cacheKey(const QString& path, const QSize& pixelSize);
};
