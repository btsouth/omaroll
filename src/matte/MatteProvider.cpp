#include "matte/MatteProvider.h"

#include "matte/MatteComposer.h"

#include <QImageReader>
#include <QRunnable>
#include <QThread>
#include <QUrl>

#include <atomic>

namespace {

// Source edge used for a preview compose. Large enough that the matte's
// gradient and the drop shadow read truthfully, small enough to be instant.
constexpr int kPreviewSourceEdge = 720;
constexpr int kFallbackEdge = 280;

class MatteResponse final : public QQuickImageResponse, public QRunnable {
public:
  MatteResponse(QString path, int matte, int aspect, qreal padding, QSize target)
      : m_path(std::move(path)), m_matte(matte), m_aspect(aspect), m_padding(padding),
        m_target(target) {
    setAutoDelete(false);
  }

  [[nodiscard]] QQuickTextureFactory* textureFactory() const override {
    return QQuickTextureFactory::textureFactoryForImage(m_image);
  }

  [[nodiscard]] QString errorString() const override { return m_error; }

  void cancel() override { m_cancelled.store(true); }

  void run() override {
    if (m_cancelled.load()) {
      m_error = QStringLiteral("Cancelled");
      emit finished();
      return;
    }
    QImageReader reader(m_path);
    reader.setAutoTransform(true);

    const QSize original = reader.size();
    if (original.isValid() && !original.isEmpty()) {
      QSize scaled = original;
      const int longest = std::max(original.width(), original.height());
      if (longest > kPreviewSourceEdge) {
        scaled = original.scaled(kPreviewSourceEdge, kPreviewSourceEdge, Qt::KeepAspectRatio);
      }
      reader.setScaledSize(scaled);
    }

    const QImage source = reader.read();
    if (source.isNull()) {
      m_error = QStringLiteral("Could not read %1").arg(m_path);
      emit finished();
      return;
    }

    QImage composed =
        MatteComposer::compose(source, static_cast<MatteComposer::Matte>(m_matte),
                               static_cast<MatteComposer::Aspect>(m_aspect), m_padding);
    if (composed.isNull()) {
      m_error = QStringLiteral("Could not compose a matte");
      emit finished();
      return;
    }

    if (m_target.isValid() && !m_target.isEmpty() &&
        (composed.width() > m_target.width() || composed.height() > m_target.height())) {
      composed = composed.scaled(m_target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    m_image = composed;
    emit finished();
  }

private:
  QString m_path;
  int m_matte = 0;
  int m_aspect = 0;
  qreal m_padding = 0.07;
  QSize m_target;
  QImage m_image;
  QString m_error;
  std::atomic_bool m_cancelled{false};
};

} // namespace

MatteProvider::MatteProvider() {
  m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() - 1, 4));
}

void MatteProvider::shutdown() {
  m_pool.clear();
  m_pool.waitForDone();
}

QQuickImageResponse* MatteProvider::requestImageResponse(const QString& id,
                                                         const QSize& requestedSize) {
  int matte = 0;
  int aspect = 0;
  qreal padding = 0.07;
  // Decoded once, then split; the QML side encodes the path whole.
  const QString decoded = QUrl::fromPercentEncoding(id.toUtf8());
  QString path = decoded;

  const qsizetype separator = decoded.indexOf(QLatin1Char('/'));
  if (separator > 0) {
    const QStringList head = decoded.left(separator).split(QLatin1Char('.'));
    if (head.size() >= 1) {
      matte = head.at(0).toInt();
    }
    if (head.size() >= 2) {
      aspect = head.at(1).toInt();
    }
    if (head.size() >= 3) {
      padding = qBound(0.0, head.at(2).toDouble() / 100.0, 0.35);
    }
    path = decoded.mid(separator);
  }

  QSize target = requestedSize;
  if (target.width() <= 0 || target.height() <= 0) {
    target = QSize(kFallbackEdge, kFallbackEdge);
  }

  auto* response = new MatteResponse(path, matte, aspect, padding, target);
  m_pool.start(response);
  return response;
}
