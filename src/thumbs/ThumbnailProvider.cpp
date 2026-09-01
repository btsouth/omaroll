#include "thumbs/ThumbnailProvider.h"

#include "thumbs/ThumbnailCache.h"

#include <QRunnable>
#include <QUrl>
#include <QThread>

namespace {

// Default cell size when QML supplies no sourceSize. Kept generous so a missing
// hint degrades to a usable tile rather than a postage stamp.
constexpr int kFallbackEdge = 320;

class ThumbnailResponse final : public QQuickImageResponse, public QRunnable {
public:
  ThumbnailResponse(QString path, QSize logicalSize, qreal devicePixelRatio, int seekPercent)
      : m_path(std::move(path)), m_logicalSize(logicalSize),
        m_devicePixelRatio(devicePixelRatio), m_seekPercent(seekPercent) {
    setAutoDelete(false);
  }

  [[nodiscard]] QQuickTextureFactory* textureFactory() const override {
    return QQuickTextureFactory::textureFactoryForImage(m_image);
  }

  [[nodiscard]] QString errorString() const override { return m_error; }

  void run() override {
    m_image = ThumbnailCache::thumbnail(m_path, m_logicalSize, m_devicePixelRatio, m_seekPercent);
    if (m_image.isNull()) {
      // A file that cannot be thumbnailed is ordinary: an unreadable codec, a
      // truncated download, a permission the user does not have. Report it and
      // let the delegate show its placeholder.
      m_error = QStringLiteral("No thumbnail for %1").arg(m_path);
    }
    emit finished();
  }

private:
  QString m_path;
  QSize m_logicalSize;
  qreal m_devicePixelRatio = 1.0;
  int m_seekPercent = 20;
  QImage m_image;
  QString m_error;
};

} // namespace

ThumbnailProvider::ThumbnailProvider() {
  // Bounded so a fast scroll through thousands of tiles cannot spawn an
  // unbounded number of ffmpegthumbnailer processes. Leave a core for the GUI
  // thread and the render thread.
  m_pool.setMaxThreadCount(std::max(2, QThread::idealThreadCount() - 1));
}

QQuickImageResponse* ThumbnailProvider::requestImageResponse(const QString& id,
                                                             const QSize& requestedSize) {
  // Ids arrive percent-encoded, so the separator has to be a character the URL
  // parser leaves alone. The ratio is the first path segment and the absolute
  // path is everything from the next slash on:
  //   image://thumbs/1.5/home/user/Pictures/shot.png
  //   image://thumbs/1.5@60/home/user/Videos/clip.mp4
  // The optional @<percent> is the seek position for a video scrub frame.
  qreal devicePixelRatio = 1.0;
  int seekPercent = 20;
  QString path = id;

  const qsizetype separator = id.indexOf(QLatin1Char('/'));
  if (separator > 0) {
    QString head = id.left(separator);

    const qsizetype at = head.indexOf(QLatin1Char('@'));
    if (at > 0) {
      bool seekOkay = false;
      const int parsedSeek = QStringView{head}.mid(at + 1).toInt(&seekOkay);
      if (seekOkay) {
        seekPercent = qBound(0, parsedSeek, 95);
      }
      head = head.left(at);
    }

    bool okay = false;
    const qreal parsed = head.toDouble(&okay);
    if (okay && parsed > 0.0) {
      devicePixelRatio = parsed;
      path = id.mid(separator);
    }
  }

  // A filename with a space or a hash reaches us encoded; decode before it is
  // ever treated as a filesystem path.
  path = QUrl::fromPercentEncoding(path.toUtf8());

  QSize logicalSize = requestedSize;
  if (logicalSize.width() <= 0 || logicalSize.height() <= 0) {
    logicalSize = QSize(kFallbackEdge, kFallbackEdge);
  }

  auto* response = new ThumbnailResponse(path, logicalSize, devicePixelRatio, seekPercent);
  m_pool.start(response);
  return response;
}
