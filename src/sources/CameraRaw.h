#pragma once

#include <QDateTime>
#include <QImage>
#include <QSize>
#include <QString>
#include <QUrl>

// Camera raw photos: ARW, CR2, CR3, DNG, NEF, RAF, ORF, RW2, PEF, SRW and the
// rest of what LibRaw reads.
//
// The pixels come from the kimageformats raw plugin, which wraps LibRaw, so
// nothing new is linked. A full demosaic of a 33-megapixel raw takes about two
// seconds, but nearly every raw carries the camera's own JPEG, often at full
// size, and the plugin hands that over in a fraction of the time. Thumbnails,
// the library and the viewer show it; the viewer decodes the raw itself only
// when that preview would be shown larger than it is.
//
// The plugin turns a full decode upright but not the preview, and ImageMagick
// reports no EXIF for raws, so the orientation and the handful of EXIF fields
// the library shows are read here, straight from the file's TIFF structure
// (or the TIFF inside a CR3 or RAF). Nothing is ever written to a raw.
namespace CameraRaw {

// A raw extension Qt can decode right now. Without LibRaw the plugin does not
// load, and raws stay unsupported files rather than becoming broken tiles.
[[nodiscard]] bool isRaw(const QString& suffix);
[[nodiscard]] bool isRawFile(const QString& path);

struct Metadata {
  // EXIF orientation, 1 to 8. 1 when the file does not say.
  int orientation = 1;
  QString make;
  QString model;
  QString lens;
  QDateTime taken;
  // As cameras state it: "1/125", or "2.5" for long exposures.
  QString exposureTime;
  double fNumber = 0;
  double focalLength = 0;
  int iso = 0;
};

// Reads only the file's headers; never decodes.
[[nodiscard]] Metadata readMetadata(const QString& path);

// The largest preview embedded in the raw, turned upright. A raw without one
// gets a half-size decode instead. With |cover|, it is first scaled down to
// just cover that size, so a thumbnail never holds a second full-size copy.
// Null when the file cannot be read.
[[nodiscard]] QImage readPreview(const QString& path, const QSize& cover = {});

// The raw itself, demosaiced at full size, 8 bits per channel, upright.
[[nodiscard]] QImage readFull(const QString& path);

// The size readFull() returns, upright, from the headers alone.
[[nodiscard]] QSize fullSize(const QString& path);

// What QML loads in place of the file, from RawImageProvider. The file's
// version (FileVersion::key, read when |version| is empty) is part of the
// URL, so a replaced file is not served from Qt's pixmap cache.
[[nodiscard]] QUrl previewUrl(const QString& path, const QString& version = {});
[[nodiscard]] QUrl fullUrl(const QString& path, const QString& version = {});

// Exposed for testing: EXIF orientation applied to an image.
[[nodiscard]] QImage upright(const QImage& image, int orientation);

} // namespace CameraRaw
