#pragma once

#include <QImage>
#include <QSize>
#include <QString>

// Produces and caches thumbnails on disk.
//
// The cache key includes the rendered pixel size, not just the file identity,
// so moving the window to a monitor with a different scale factor regenerates
// rather than upscaling a thumbnail that was correct on the old one.
//
// Thumbnails cover the requested size rather than fit inside it: the short
// side matches the tile and the long side overhangs. The grid crops the
// overhang, so a 16:9 frame in a 3:2 cell is never stretched up to fill it,
// and a view that fits instead of cropping simply scales the whole thing down.
class ThumbnailCache {
public:
  // Hard ceiling on device pixel ratio. Past 2x the extra pixels cost decode
  // time and cache space without being visible.
  static constexpr qreal kMaxDevicePixelRatio = 2.0;

  // seekPercent only applies to video: which point in the clip to grab. Frame
  // zero is very often black, so the resting value is a fifth of the way in.
  [[nodiscard]] static QImage thumbnail(const QString& path, const QSize& logicalSize,
                                        qreal devicePixelRatio, int seekPercent = 20);

  [[nodiscard]] static QString cacheDirectory();

  // Cache ceiling. A thumbnail is always rebuildable, so bounding the directory
  // costs a little work later rather than losing anything.
  static constexpr qint64 kMaxCacheBytes = 256LL * 1024 * 1024;

  // Deletes least-recently-used entries until the directory is under the
  // ceiling. Safe to call from a worker thread; does nothing when already under.
  static void prune(qint64 maxBytes = kMaxCacheBytes);

private:
  [[nodiscard]] static QImage renderImage(const QString& path, const QSize& pixelSize);
  [[nodiscard]] static QImage renderVideo(const QString& path, const QSize& pixelSize,
                                          int seekPercent);
  [[nodiscard]] static QString cacheKey(const QString& path, const QSize& pixelSize,
                                        int seekPercent);
};
