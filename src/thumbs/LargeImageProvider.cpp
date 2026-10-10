#include "thumbs/LargeImageProvider.h"

#include "sources/ImageBudget.h"

#include <QImageReader>
#include <QMetaObject>
#include <QRunnable>
#include <QUrl>

namespace {

constexpr int kFallbackEdge = 4096;

class LargeImageResponse final : public QQuickImageResponse, public QRunnable {
public:
  LargeImageResponse(QString path, int edge, std::shared_ptr<std::atomic_bool> stopping)
      : m_path(std::move(path)), m_edge(edge), m_stopping(std::move(stopping)) {
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

  void run() override {
    if (!m_cancelled.load() && !m_stopping->load()) m_image = read();
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
    const auto targetFor = [&estimate](int edge) {
      return estimate.oriented.scaled(edge, edge, Qt::KeepAspectRatio)
          .boundedTo(estimate.oriented).expandedTo(QSize(1, 1));
    };
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
      return reader.read();
    }
    // Takes its turn with background work decoding very large pictures.
    QImage image = ImageBudget::readBounded(
        reader, [this] { return m_cancelled.load() || m_stopping->load(); });
    if (image.isNull()) {
      m_error = reader.errorString();
      return {};
    }
    if (image.width() > target.width() || image.height() > target.height()) {
      image = image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
  }

  QString m_path;
  int m_edge = kFallbackEdge;
  QImage m_image;
  QString m_error;
  std::atomic_bool m_cancelled{false};
  std::shared_ptr<std::atomic_bool> m_stopping;
};

} // namespace

LargeImageProvider::LargeImageProvider() { m_pool.setMaxThreadCount(1); }

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
  bool ok = false;
  int edge = head.section(QLatin1Char('~'), 0, 0).toInt(&ok);
  if (!ok || edge <= 0) edge = kFallbackEdge;
  if (requestedSize.isValid() && !requestedSize.isEmpty()) {
    edge = std::min(edge, std::max(requestedSize.width(), requestedSize.height()));
  }
  auto* response = new LargeImageResponse(separator > 0 ? decoded.mid(separator) : QString(),
                                          edge, m_stopping);
  m_pool.start(response);
  return response;
}
