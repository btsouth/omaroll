#include "sources/ImageBudget.h"

#include "sources/FileVersion.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImageIOHandler>
#include <QImageReader>
#include <QLocale>
#include <QScreen>
#include <QSemaphore>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include <unistd.h>

namespace ImageBudget {

namespace {

// Read by decoders on worker threads; written at startup and by tests.
std::atomic<qint64> g_backgroundBytes{Limits{}.backgroundBytes};
std::atomic<qint64> g_displayBytes{Limits{}.displayBytes};
std::atomic<int> g_textureEdge{Limits{}.textureEdge};
// The one decode past the background budget allowed at a time.
QSemaphore g_largeDecode(1);

constexpr int kMinimumLimitMb = 256;
constexpr int kMaximumLimitMb = 2048;
// The reduced copy's longest side. Qt reduces a JPEG by eighths to the first
// size at least the copy's, so at most 6144 its decode stays far under Qt's
// default limit for any picture short of gigapixels.
constexpr int kMinimumReducedEdge = 2048;
constexpr int kMaximumReducedEdge = 6144;

// The allocation Qt's decoders make and check: rows padded to 32 bits. A
// hostile header saturates rather than wrapping.
qint64 imageBytes(const QSize& size, int depth) {
  if (size.width() <= 0 || size.height() <= 0 || depth <= 0) return -1;
  const qint64 bytesPerLine = ((qint64(size.width()) * depth + 31) >> 5) << 2;
  if (bytesPerLine > std::numeric_limits<qint64>::max() / size.height()) {
    return std::numeric_limits<qint64>::max();
  }
  return bytesPerLine * size.height();
}

int depthOf(QImageReader& reader, bool* known) {
  const QImage::Format format = reader.imageFormat();
  const int depth = format == QImage::Format_Invalid ? 0 : QImage::toPixelFormat(format).bitsPerPixel();
  *known = depth > 0;
  return depth > 0 ? depth : 32;
}

} // namespace

Limits limits() {
  return {g_backgroundBytes.load(), g_displayBytes.load(), g_textureEdge.load()};
}

void setLimits(const Limits& limits) {
  g_backgroundBytes.store(limits.backgroundBytes);
  g_displayBytes.store(limits.displayBytes);
  g_textureEdge.store(limits.textureEdge);
}

qint64 decodeLimitBytes() { return qint64(std::max(0, QImageReader::allocationLimit())) * kMiB; }

int automaticLimitMb(qint64 physicalBytes) {
  if (physicalBytes <= 0) return kMinimumLimitMb;
  return int(std::clamp<qint64>(physicalBytes / 8 / kMiB, kMinimumLimitMb, kMaximumLimitMb));
}

int startupLimitMb(const QByteArray& environment, qint64 physicalBytes) {
  // The same parse Qt makes of the variable.
  bool ok = false;
  const int requested = environment.trimmed().toInt(&ok, 0);
  if (ok && requested >= 0) return -1;
  return automaticLimitMb(physicalBytes);
}

void applyStartupLimit() {
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long pageSize = ::sysconf(_SC_PAGESIZE);
  const qint64 physical = pages > 0 && pageSize > 0 ? qint64(pages) * pageSize : 0;
  const int limit = startupLimitMb(qgetenv("QT_IMAGEIO_MAXALLOC"), physical);
  if (limit >= 0) QImageReader::setAllocationLimit(limit);
}

Estimate estimate(QImageReader& reader) {
  Estimate result;
  const QSize size = reader.size();
  if (size.width() <= 0 || size.height() <= 0) return result;
  result.stored = size;
  result.oriented = reader.transformation() & QImageIOHandler::TransformationRotate90
                        ? size.transposed() : size;
  result.depth = depthOf(reader, &result.formatKnown);
  result.bytes = imageBytes(size, result.depth);
  result.scalable = reader.supportsOption(QImageIOHandler::ScaledSize);
  result.jpeg = reader.format() == "jpeg";
  return result;
}

qint64 bytesAt(const Estimate& estimate, const QSize& scaled) {
  if (!estimate.valid()) return -1;
  if (!estimate.scalable || !scaled.isValid() || scaled.isEmpty()) return estimate.bytes;
  if (!estimate.jpeg) return imageBytes(scaled, estimate.depth);
  // As qjpeghandler chooses libjpeg's scale_num over a denominator of 8.
  const double factor = std::min(double(estimate.stored.width()) / scaled.width(),
                                 double(estimate.stored.height()) / scaled.height());
  const int eighths = std::clamp(int(std::ceil(8 / factor)), 1, 8);
  const QSize decoded((qint64(estimate.stored.width()) * eighths + 7) / 8,
                      (qint64(estimate.stored.height()) * eighths + 7) / 8);
  return imageBytes(decoded, estimate.depth);
}

Estimate estimate(const QString& path) {
  QImageReader reader(path);
  reader.setAutoTransform(true);
  return estimate(reader);
}

bool fitsBudget(QImageReader& reader, qint64 budget) {
  const Estimate full = estimate(reader);
  return !full.valid() || budget <= 0 || bytesAt(full, reader.scaledSize()) <= budget;
}

LargeDecode::LargeDecode(QImageReader& reader, const std::function<bool()>& cancelled) {
  const Estimate full = estimate(reader);
  if (!full.valid()) return;
  const qint64 bytes = bytesAt(full, reader.scaledSize());
  if (bytes <= limits().backgroundBytes) return;
  const qint64 limit = decodeLimitBytes();
  if (limit > 0 && bytes > limit) {
    m_allowed = false;
    return;
  }
  while (!g_largeDecode.tryAcquire(1, 50)) {
    if (cancelled && cancelled()) {
      m_allowed = false;
      return;
    }
  }
  m_held = true;
}

LargeDecode::~LargeDecode() {
  if (m_held) g_largeDecode.release();
}

QImage readBounded(QImageReader& reader, const std::function<bool()>& cancelled) {
  const LargeDecode decode(reader, cancelled);
  return decode.allowed() ? reader.read() : QImage();
}

QString formatBytes(qint64 bytes) {
  return QLocale().formattedDataSize(bytes, bytes >= 1024 * kMiB ? 1 : 0,
                                     QLocale::DataSizeIecFormat);
}

Plan plan(const Estimate& estimate) {
  Plan result;
  if (!estimate.valid()) return result;
  const Limits current = limits();
  const qint64 limit = decodeLimitBytes();
  const int longest = std::max(estimate.stored.width(), estimate.stored.height());
  // A lower limit set through QT_IMAGEIO_MAXALLOC also makes a picture large.
  const qint64 threshold = limit > 0 ? std::min(current.displayBytes, limit) : current.displayBytes;
  if (estimate.bytes <= threshold && longest <= current.textureEdge) return result;

  result.reduced = true;
  result.size = estimate.oriented;
  const bool fits = limit == 0 || estimate.bytes <= limit;
  if (!fits && !estimate.scalable) {
    result.detail = false;
    result.error = decodeRefusal(estimate);
    return result;
  }
  result.detail = fits && longest <= current.textureEdge;
  if (!fits) {
    result.note = QStringLiteral("Showing a reduced copy. Full resolution needs about %1; the "
                                 "limit on this computer is %2")
                      .arg(formatBytes(estimate.bytes), formatBytes(limit));
  } else if (!result.detail) {
    result.edge = current.textureEdge;
    result.note = QStringLiteral("Showing a reduced copy. Full resolution is too large for the "
                                 "graphics card");
  }
  return result;
}

QString decodeRefusal(const Estimate& estimate) {
  const qint64 limit = decodeLimitBytes();
  if (!estimate.valid() || limit == 0 || estimate.bytes <= limit) return {};
  return QStringLiteral("This picture needs about %1; the limit on this computer is %2")
      .arg(formatBytes(estimate.bytes), formatBytes(limit));
}

QString previewRefusal(const Estimate& estimate) {
  return estimate.scalable ? QString() : decodeRefusal(estimate);
}

int reducedEdge(const Plan& plan) {
  if (plan.edge > 0) return plan.edge;
  qreal longest = 0;
  if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    for (const QScreen* screen : QGuiApplication::screens()) {
      const QSize size = screen->geometry().size();
      longest = std::max(longest, std::max(size.width(), size.height()) * screen->devicePixelRatio());
    }
  }
  return std::clamp(int(std::ceil(longest)), kMinimumReducedEdge, kMaximumReducedEdge);
}

QUrl reducedUrl(const QString& path, const QString& version, int edge) {
  if (path.isEmpty()) return {};
  // The same shape as the raw and thumbnail ids: a head without a slash,
  // then the absolute path encoded whole.
  return QUrl(QStringLiteral("image://large/%1~%2")
                  .arg(edge)
                  .arg(version.isEmpty() ? FileVersion::key(path) : version) +
              QString::fromLatin1(QUrl::toPercentEncoding(path)));
}

} // namespace ImageBudget
