#include "edit/EditProvider.h"

#include "edit/ImageEditor.h"

#include <QImageReader>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>
#include <QUrl>

#include <atomic>

namespace {

// Source edge decoded for the editor preview. Larger than the matte picker's:
// the editor is the whole window, and crop handles want detail under them.
constexpr int kPreviewSourceEdge = 1400;
constexpr int kFallbackEdge = 720;

class EditResponse final : public QQuickImageResponse, public QRunnable {
public:
  EditResponse(QString path, int quarterTurns, bool flipHorizontal, bool flipVertical, QSize target)
      : m_path(std::move(path)), m_quarterTurns(quarterTurns), m_flipH(flipHorizontal),
        m_flipV(flipVertical), m_target(target) {
    setAutoDelete(false);
  }

  [[nodiscard]] QQuickTextureFactory* textureFactory() const override {
    return QQuickTextureFactory::textureFactoryForImage(m_image);
  }

  [[nodiscard]] QString errorString() const override { return m_error; }

  void cancel() override { m_cancelled.store(true); }

  // Emitting straight from the pool thread races the pixmap reader's
  // deleteLater(); post the emit to the response's own thread instead, as the
  // thumbnail and matte providers do.
  void finishOnOwnThread() {
    QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
  }

  void run() override {
    if (m_cancelled.load()) {
      m_error = QStringLiteral("Cancelled");
      finishOnOwnThread();
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
      finishOnOwnThread();
      return;
    }

    ImageEditor::Transform transform;
    transform.quarterTurns = m_quarterTurns;
    transform.flipHorizontal = m_flipH;
    transform.flipVertical = m_flipV;
    QImage preview = ImageEditor::apply(source, transform);
    if (preview.isNull()) {
      m_error = QStringLiteral("Could not preview this image");
      finishOnOwnThread();
      return;
    }

    if (m_target.isValid() && !m_target.isEmpty() &&
        (preview.width() > m_target.width() || preview.height() > m_target.height())) {
      preview = preview.scaled(m_target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    m_image = preview;
    finishOnOwnThread();
  }

private:
  QString m_path;
  int m_quarterTurns = 0;
  bool m_flipH = false;
  bool m_flipV = false;
  QSize m_target;
  QImage m_image;
  QString m_error;
  std::atomic_bool m_cancelled{false};
};

} // namespace

EditProvider::EditProvider() {
  m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() - 1, 4));
}

void EditProvider::shutdown() {
  m_pool.clear();
  m_pool.waitForDone();
}

QQuickImageResponse* EditProvider::requestImageResponse(const QString& id,
                                                        const QSize& requestedSize) {
  int quarterTurns = 0;
  bool flipH = false;
  bool flipV = false;
  const QString decoded = QUrl::fromPercentEncoding(id.toUtf8());
  QString path = decoded;

  const qsizetype separator = decoded.indexOf(QLatin1Char('/'));
  if (separator > 0) {
    const QStringList head = decoded.left(separator).split(QLatin1Char('.'));
    if (head.size() >= 1) {
      quarterTurns = head.at(0).toInt();
    }
    if (head.size() >= 2) {
      flipH = head.at(1) == QLatin1String("1");
    }
    if (head.size() >= 3) {
      flipV = head.at(2) == QLatin1String("1");
    }
    path = decoded.mid(separator);
  }

  QSize target = requestedSize;
  if (target.width() <= 0 || target.height() <= 0) {
    target = QSize(kFallbackEdge, kFallbackEdge);
  }

  auto* response = new EditResponse(path, quarterTurns, flipH, flipV, target);
  m_pool.start(response);
  return response;
}
