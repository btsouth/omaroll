#include "thumbs/RawImageProvider.h"

#include "sources/CameraRaw.h"

#include <QMetaObject>
#include <QRunnable>
#include <QUrl>

#include <atomic>
#include <memory>

namespace {

class RawImageResponse final : public QQuickImageResponse, public QRunnable {
public:
  RawImageResponse(QString path, bool full, QSize requestedSize,
                   std::shared_ptr<std::atomic_bool> stopping)
      : m_path(std::move(path)), m_full(full), m_requestedSize(requestedSize),
        m_stopping(std::move(stopping)) {
    setAutoDelete(false);
  }

  [[nodiscard]] QQuickTextureFactory* textureFactory() const override {
    return QQuickTextureFactory::textureFactoryForImage(m_image);
  }

  [[nodiscard]] QString errorString() const override { return m_error; }

  // Stepping quickly through a folder moves past previews before they are
  // read; skipping them keeps the pool on the picture being looked at.
  void cancel() override { m_cancelled.store(true); }

  // Emitted on the response's own thread for the same reason as in
  // ThumbnailProvider: Qt deletes the response as soon as finished() lands.
  void finishOnOwnThread() {
    QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
  }

  void run() override {
    if (!m_cancelled.load() && !m_stopping->load()) {
      m_image = m_full ? CameraRaw::readFull(m_path)
                       : CameraRaw::readPreview(m_path, m_requestedSize);
      if (!m_image.isNull() && m_requestedSize.isValid() && !m_requestedSize.isEmpty() &&
          (m_image.width() > m_requestedSize.width() ||
           m_image.height() > m_requestedSize.height())) {
        m_image = m_image.scaled(m_requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      }
    }
    if (m_image.isNull()) {
      m_error = m_cancelled.load() || m_stopping->load()
                    ? QStringLiteral("Cancelled")
                                   : QStringLiteral("Could not decode %1").arg(m_path);
    }
    finishOnOwnThread();
  }

private:
  QString m_path;
  bool m_full = false;
  QSize m_requestedSize;
  QImage m_image;
  QString m_error;
  std::atomic_bool m_cancelled {false};
  std::shared_ptr<std::atomic_bool> m_stopping;
};

} // namespace

RawImageProvider::RawImageProvider() {
  // The picture on screen and one neighbour either side.
  m_previews.setMaxThreadCount(3);
  m_full.setMaxThreadCount(1);
}

RawImageProvider::~RawImageProvider() { shutdown(); }

void RawImageProvider::shutdown() {
  // Responses are Qt's until finished(), queued ones included, so they are
  // drained with the stop flag rather than dropped with clear(). A decode
  // already under way runs to its end.
  m_stopping->store(true);
  m_previews.waitForDone();
  m_full.waitForDone();
}

QQuickImageResponse* RawImageProvider::requestImageResponse(const QString& id,
                                                            const QSize& requestedSize) {
  // "<variant>~<mtime>" then the path from its first slash, decoded once as
  // the thumbnail ids are. The stamp only keeps the URL unique per version.
  const QString decoded = QUrl::fromPercentEncoding(id.toUtf8());
  const qsizetype separator = decoded.indexOf(QLatin1Char('/'));
  const QString head = separator > 0 ? decoded.left(separator) : QString();
  const bool full = head.startsWith(QStringLiteral("full"));
  auto* response =
      new RawImageResponse(separator > 0 ? decoded.mid(separator) : QString(), full,
                           requestedSize, m_stopping);
  (full ? m_full : m_previews).start(response);
  return response;
}
