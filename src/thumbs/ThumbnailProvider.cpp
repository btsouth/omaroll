#include "thumbs/ThumbnailProvider.h"

#include "thumbs/ThumbnailCache.h"

#include <QRunnable>
#include <QThread>
#include <QUrl>

#include <atomic>

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

  // Qt cancels when the requesting Image goes away or changes source, which a
  // fast scroll does constantly. Skipping the work keeps the pool on tiles
  // that are still on screen.
  void cancel() override { m_cancelled.store(true); }

  void run() override {
    if (m_cancelled.load()) {
      m_error = QStringLiteral("Cancelled");
      emit finished();
      return;
    }
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
  std::atomic_bool m_cancelled{false};
};

} // namespace

ThumbnailProvider::ThumbnailProvider() {
  // Bounded so a fast scroll through thousands of tiles cannot spawn an
  // unbounded number of ffmpegthumbnailer processes. Leave a core for the GUI
  // thread and the render thread.
  m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() - 1, 4));
}

void ThumbnailProvider::shutdown() {
  m_pool.clear();
  m_pool.waitForDone();
}

QQuickImageResponse* ThumbnailProvider::requestImageResponse(const QString& id,
                                                             const QSize& requestedSize) {
  // Ids arrive percent-encoded, so the separator has to be a character the URL
  // parser leaves alone. The ratio is the first path segment and the absolute
  // path is everything from the next slash on:
  //   image://thumbs/1.5/home/user/Pictures/shot.png
  //   image://thumbs/1.5@60~1725200000/home/user/Videos/clip.mp4
  // The optional @<percent> is the seek position for a video scrub frame. The
  // optional ~<stamp> is the file's mtime: it is not used here, it only makes
  // the URL differ when the file is rewritten, so Qt's in-memory pixmap cache
  // cannot keep serving the old frame.
  qreal devicePixelRatio = 1.0;
  int seekPercent = 20;
  // QML percent-encodes the path whole, so a '#', a '?' or a literal '%' in
  // a filename survives URL parsing. Decode once, then split: the head never
  // contains a slash, so the first one starts the path.
  const QString decoded = QUrl::fromPercentEncoding(id.toUtf8());
  QString path = decoded;

  const qsizetype separator = decoded.indexOf(QLatin1Char('/'));
  if (separator > 0) {
    QString head = decoded.left(separator);

    const qsizetype tilde = head.indexOf(QLatin1Char('~'));
    if (tilde > 0) {
      head = head.left(tilde);
    }

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
      path = decoded.mid(separator);
    }
  }

  QSize logicalSize = requestedSize;
  if (logicalSize.width() <= 0 || logicalSize.height() <= 0) {
    logicalSize = QSize(kFallbackEdge, kFallbackEdge);
  }

  auto* response = new ThumbnailResponse(path, logicalSize, devicePixelRatio, seekPercent);
  m_pool.start(response);
  return response;
}
