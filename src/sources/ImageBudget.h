#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QUrl>

#include <functional>

class QImageReader;

// How much memory a picture costs to decode, and what Omaroll does about it.
//
// Qt refuses any decode larger than QImageReader's allocation limit, which is
// global to the process. At startup Omaroll raises it from Qt's 256 MiB to a
// share of this computer's memory. Background work (thumbnails, similarity,
// previews in sheets) runs on several workers at once, and a format that
// cannot decode at a reduced size (PNG, TIFF, WebP and others) decodes whole
// before it is scaled, so past the old 256 MiB those decodes take turns: one
// at a time, process-wide. JPEG decodes at a reduced size and rarely needs to.
//
// The viewers show a very large picture as a copy the size of the screen, at
// its true dimensions, and only load full resolution once zoomed past that
// copy. A full decode never reaches the graphics card unless it fits.
namespace ImageBudget {

inline constexpr qint64 kMiB = 1024 * 1024;

struct Limits {
  // A full decode in background work, as Qt's default allowed before.
  qint64 backgroundBytes = 256 * kMiB;
  // Larger than this, a viewer shows a reduced copy first.
  qint64 displayBytes = 256 * kMiB;
  // A side longer than this is never uploaded at full resolution. Most
  // graphics drivers allow 16384; the reduced copy stays well under it.
  int textureEdge = 16384;
};

[[nodiscard]] Limits limits();
// For tests, which use small pictures and small limits.
void setLimits(const Limits& limits);

// The limit Qt applies to every decode, in bytes. Zero means none.
[[nodiscard]] qint64 decodeLimitBytes();

// An eighth of physical memory, between 256 MiB and 2 GiB.
[[nodiscard]] int automaticLimitMb(qint64 physicalBytes);
// QT_IMAGEIO_MAXALLOC wins when it is a number; Qt reads it itself then, so
// this returns -1. Otherwise the automatic limit.
[[nodiscard]] int startupLimitMb(const QByteArray& environment, qint64 physicalBytes);
// Call once, before anything is decoded.
void applyStartupLimit();

struct Estimate {
  // As stored, and as shown once the EXIF orientation is applied.
  QSize stored;
  QSize oriented;
  // What a full decode allocates, the way Qt measures it. Pictures whose
  // pixel format is unknown until decoded (WebP, AVIF, JPEG XL) count as
  // 8-bit RGBA.
  qint64 bytes = -1;
  int depth = 32;
  bool formatKnown = false;
  // The decoder can produce a reduced size without a full decode first.
  bool scalable = false;
  bool jpeg = false;

  [[nodiscard]] bool valid() const { return bytes >= 0; }
};

// Reads only the header.
[[nodiscard]] Estimate estimate(QImageReader& reader);
[[nodiscard]] Estimate estimate(const QString& path);

// What a read at |scaled| (as stored, before orientation) allocates. Qt's
// JPEG decoder reduces by eighths, from 1/8 to the whole picture, to the
// first size at least |scaled|, then scales the rest of the way; a target
// over half the picture can mean a whole decode. Other formats that cannot
// reduce decode whole.
[[nodiscard]] qint64 bytesAt(const Estimate& estimate, const QSize& scaled);

// Whether a read of |reader| allocates at most |budget|. A scaled size set on
// a decoder that supports it is measured at that size. Unknown sizes pass:
// Qt's own limit still applies to them.
[[nodiscard]] bool fitsBudget(QImageReader& reader, qint64 budget);

// Permission for background work to read |reader|. Within the background
// budget it is given at once. Past it, within Qt's limit, the reader waits
// for the one slot the whole process has for such decodes, so several
// workers never hold several very large pictures at once; |cancelled| is
// polled meanwhile. Past Qt's limit it is refused.
class LargeDecode {
public:
  explicit LargeDecode(QImageReader& reader, const std::function<bool()>& cancelled = {});
  ~LargeDecode();
  LargeDecode(const LargeDecode&) = delete;
  LargeDecode& operator=(const LargeDecode&) = delete;

  [[nodiscard]] bool allowed() const { return m_allowed; }

private:
  bool m_allowed = true;
  bool m_held = false;
};

// Reads |reader| with a LargeDecode's permission. Null when refused,
// cancelled or unreadable.
[[nodiscard]] QImage readBounded(QImageReader& reader, const std::function<bool()>& cancelled = {});

// "555 MiB", "2.1 GiB".
[[nodiscard]] QString formatBytes(qint64 bytes);

// What a viewer should do with one picture.
struct Plan {
  // Show the reduced copy first, measured at |size|.
  bool reduced = false;
  QSize size;
  // The copy's longest side, when not the screen's: a picture that fits in
  // memory but is too long for a texture gets the longest one there can be.
  int edge = 0;
  // Full resolution can be loaded when zoomed in.
  bool detail = true;
  // Said once the picture is zoomed past the reduced copy without detail.
  QString note;
  // Set when not even the reduced copy can be decoded.
  QString error;
};

[[nodiscard]] Plan plan(const Estimate& estimate);

// Why a full decode of this picture is refused by Qt's limit, or empty.
[[nodiscard]] QString decodeRefusal(const Estimate& estimate);
// Why a preview in a sheet (corrections, mattes) cannot be decoded, or
// empty. A JPEG's preview decodes reduced and always can.
[[nodiscard]] QString previewRefusal(const Estimate& estimate);

// The longest side of the reduced copy: |plan|'s own, or else the largest
// screen's in device pixels, within bounds that keep its decode small.
// Reads the screens, so call it on the GUI thread.
[[nodiscard]] int reducedEdge(const Plan& plan = {});

// What QML loads for the reduced copy, from LargeImageProvider. The file's
// version keeps a replaced file out of Qt's pixmap cache.
[[nodiscard]] QUrl reducedUrl(const QString& path, const QString& version, int edge);

} // namespace ImageBudget
