#include "thumbs/LargeImageProvider.h"

#include "sources/DisplayColors.h"
#include "sources/ImageBudget.h"

#include <QImageReader>
#include <QMetaObject>
#include <QRunnable>
#include <QTransform>
#include <QUrl>

#include <algorithm>

namespace {

constexpr int kFallbackEdge = 4096;

// Maps a stored picture of |stored| size to its upright orientation. Qt
// mirrors first, then turns a quarter clockwise.
QTransform uprightTransform(const QSize& stored, QImageIOHandler::Transformations transformation) {
  QTransform mirror;
  if (transformation & QImageIOHandler::TransformationMirror) {
    mirror = QTransform(-1, 0, 0, 1, stored.width(), 0);
  }
  if (transformation & QImageIOHandler::TransformationFlip) {
    mirror *= QTransform(1, 0, 0, -1, 0, stored.height());
  }
  if (!(transformation & QImageIOHandler::TransformationRotate90)) return mirror;
  return mirror * QTransform(0, 1, -1, 0, stored.height(), 0);
}

class LargeImageResponse final : public QQuickImageResponse, public QRunnable {
public:
  LargeImageResponse(QString path, int edge, std::shared_ptr<std::atomic_bool> stopping)
      : m_path(std::move(path)), m_edge(edge), m_stopping(std::move(stopping)) {
    setAutoDelete(false);
  }

  LargeImageResponse(QString path, const QRect& area, const QSize& size,
                     std::shared_ptr<std::atomic_bool> stopping)
      : m_path(std::move(path)), m_area(area), m_size(size), m_stopping(std::move(stopping)) {
    setAutoDelete(false);
  }

  [[nodiscard]] QQuickTextureFactory* textureFactory() const override {
    return QQuickTextureFactory::textureFactoryForImage(m_image);
  }

  [[nodiscard]] QString errorString() const override { return m_error; }

  void cancel() override { m_cancelled.store(true); }

  // Emitted on the response's own thread for the same reason as in
  // ThumbnailProvider: Qt deletes the response as soon as finished() lands.
  void finishOnOwnThread() {
    QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
  }

  void reject(const QString& error) {
    m_error = error;
    finishOnOwnThread();
  }

  void run() override {
    if (!m_cancelled.load() && !m_stopping->load()) {
      m_image = m_area.isValid() ? readArea() : read();
    }
    if (m_image.isNull() && m_error.isEmpty()) {
      m_error = m_cancelled.load() || m_stopping->load()
                    ? QStringLiteral("Cancelled")
                    : QStringLiteral("Could not decode %1").arg(m_path);
    }
    finishOnOwnThread();
  }

private:
  QImage read() {
    QImageReader reader(m_path);
    reader.setAutoTransform(true);
    const ImageBudget::Estimate estimate = ImageBudget::estimate(reader);
    if (!estimate.valid()) return {};
    // The scaled size is applied before the EXIF orientation is.
    const bool sideways = estimate.oriented != estimate.stored;
    const auto targetFor = [&estimate](int edge) { return ImageBudget::reducedSize(estimate, edge); };
    QSize target = targetFor(m_edge);
    if (estimate.scalable) {
      // Under a low QT_IMAGEIO_MAXALLOC even the reduced decode can be
      // refused; a smaller copy is better than none.
      const qint64 limit = ImageBudget::decodeLimitBytes();
      for (int edge = m_edge; limit > 0 && edge > 256; edge = edge * 3 / 4) {
        target = targetFor(edge);
        if (ImageBudget::bytesAt(estimate, sideways ? target.transposed() : target) <= limit) break;
      }
      reader.setScaledSize(sideways ? target.transposed() : target);
    }
    // Takes its turn with background work decoding very large pictures.
    QImage image = ImageBudget::readBounded(
        reader, [this] { return m_cancelled.load() || m_stopping->load(); });
    if (image.isNull()) {
      m_error = reader.errorString();
      return {};
    }
    // The target already has the picture's shape; keeping the aspect ratio
    // again would round a pixel off.
    if (image.width() > target.width() || image.height() > target.height()) {
      image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    convertToDisplayColors(image);
    return image;
  }

  // Part of the upright picture, decoded at |m_size| at most and never past
  // its own pixels. A JPEG decodes only the rows down to it, at an eighth,
  // quarter or half when that is enough, so this costs about as much memory
  // as the screen at any zoom.
  QImage readArea() {
    QImageReader reader(m_path);
    reader.setAutoTransform(false);
    const QSize stored = reader.size();
    if (stored.isEmpty()) {
      m_error = reader.errorString();
      return {};
    }
    const QTransform upright = uprightTransform(stored, reader.transformation());
    const QRect clip = upright.inverted().mapRect(QRectF(m_area)).toAlignedRect()
                       & QRect(QPoint(), stored);
    if (clip.isEmpty()) return {};
    const bool sideways = upright.m11() == 0;
    const QSize wanted = clip.size().scaled((sideways ? m_size.transposed() : m_size)
                                                .boundedTo(clip.size()),
                                            Qt::KeepAspectRatio);
    // Qt reduces a clipped JPEG only when asked for the whole picture at
    // the reduced size and the clip within that; a clip at full size with a
    // smaller output decodes the clip whole first.
    int reduction = 1;
    while (reduction < 8 && clip.width() / (reduction * 2) >= wanted.width() &&
           clip.height() / (reduction * 2) >= wanted.height()) {
      reduction *= 2;
    }
    if (reduction == 1) {
      reader.setClipRect(clip);
    } else {
      const auto reduce = [reduction](int value) { return (value + reduction - 1) / reduction; };
      reader.setScaledSize(QSize(reduce(stored.width()), reduce(stored.height())));
      const QRect scaled(clip.x() / reduction, clip.y() / reduction,
                         std::max(1, clip.width() / reduction),
                         std::max(1, clip.height() / reduction));
      reader.setScaledClipRect(scaled & QRect(QPoint(), reader.scaledSize()));
    }
    // What this allocates is the clip at that size, within Qt's limit, so it
    // needs none of the large decode's turns.
    if (m_cancelled.load() || m_stopping->load()) return {};
    QImage image = reader.read();
    if (image.isNull()) {
      m_error = reader.errorString();
      return {};
    }
    if (image.size() != wanted) {
      image = image.scaled(wanted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (!upright.isIdentity()) image = image.transformed(upright, Qt::FastTransformation);
    convertToDisplayColors(image);
    return image;
  }

  QString m_path;
  int m_edge = kFallbackEdge;
  QRect m_area;
  QSize m_size;
  QImage m_image;
  QString m_error;
  std::atomic_bool m_cancelled{false};
  std::shared_ptr<std::atomic_bool> m_stopping;
};

} // namespace

LargeImageProvider::LargeImageProvider() { m_pool.setMaxThreadCount(2); }

LargeImageProvider::~LargeImageProvider() { shutdown(); }

void LargeImageProvider::shutdown() {
  // As in RawImageProvider: responses belong to Qt until finished(), so they
  // are drained with the stop flag rather than dropped.
  m_stopping->store(true);
  m_pool.waitForDone();
}

QQuickImageResponse* LargeImageProvider::requestImageResponse(const QString& id,
                                                              const QSize& requestedSize) {
  // "<edge>~<version>" then the path from its first slash, decoded once.
  const QString decoded = QUrl::fromPercentEncoding(id.toUtf8());
  const qsizetype separator = decoded.indexOf(QLatin1Char('/'));
  const QString head = separator > 0 ? decoded.left(separator) : QString();
  const QString path = separator > 0 ? decoded.mid(separator) : QString();
  // Or "<x>,<y>,<width>,<height>,<output width>,<output height>~<version>".
  const QStringList area = head.section(QLatin1Char('~'), 0, 0).split(QLatin1Char(','));
  if (area.size() == 6) {
    int values[6] = {};
    bool valid = true;
    for (int i = 0; i < 6 && valid; ++i) values[i] = area[i].toInt(&valid);
    const QRect rect(values[0], values[1], values[2], values[3]);
    const QSize size(values[4], values[5]);
    auto* response = new LargeImageResponse(path, rect, size, m_stopping);
    if (!valid || rect.isEmpty() || size.isEmpty()) {
      response->reject(QStringLiteral("Not an area of %1").arg(path));
      return response;
    }
    m_pool.start(response);
    return response;
  }
  bool ok = false;
  int edge = head.section(QLatin1Char('~'), 0, 0).toInt(&ok);
  if (!ok || edge <= 0) edge = kFallbackEdge;
  if (requestedSize.isValid() && !requestedSize.isEmpty()) {
    edge = std::min(edge, std::max(requestedSize.width(), requestedSize.height()));
  }
  auto* response = new LargeImageResponse(path, edge, m_stopping);
  m_pool.start(response);
  return response;
}
