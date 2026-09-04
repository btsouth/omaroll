#include "pdf/PdfProvider.h"

#include "pdf/PdfSupport.h"

#include <QRunnable>
#include <QThread>
#include <QUrl>

#include <atomic>

namespace {
class PdfResponse final : public QQuickImageResponse, public QRunnable {
public:
  PdfResponse(QString path, int page, QSize target)
      : m_path(std::move(path)), m_page(page), m_target(target) {
    setAutoDelete(false);
  }
  QQuickTextureFactory* textureFactory() const override {
    return QQuickTextureFactory::textureFactoryForImage(m_image);
  }
  QString errorString() const override { return m_error; }
  void cancel() override { m_cancelled.store(true); }
  void run() override {
    if (m_cancelled.load()) {
      m_error = QStringLiteral("Cancelled");
      emit finished();
      return;
    }
    m_image = PdfSupport::renderPage(m_path, m_page, m_target);
    if (m_image.isNull()) {
      m_error = QStringLiteral("Could not render PDF page");
    }
    emit finished();
  }

private:
  QString m_path;
  int m_page = 1;
  QSize m_target;
  QImage m_image;
  QString m_error;
  std::atomic_bool m_cancelled{false};
};
} // namespace

PdfProvider::PdfProvider() {
  m_pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() - 1, 3));
}

void PdfProvider::shutdown() {
  m_pool.clear();
  m_pool.waitForDone();
}

QQuickImageResponse* PdfProvider::requestImageResponse(const QString& id,
                                                       const QSize& requestedSize) {
  const QString decoded = QUrl::fromPercentEncoding(id.toUtf8());
  QString path = decoded;
  int page = 1;
  const qsizetype separator = decoded.indexOf(QLatin1Char('/'));
  if (separator > 0) {
    bool okay = false;
    page = decoded.left(separator).section(QLatin1Char('~'), 0, 0).toInt(&okay);
    if (!okay) {
      page = 1;
    }
    path = decoded.mid(separator);
  }
  QSize target = requestedSize;
  if (!target.isValid() || target.isEmpty()) {
    target = QSize(1200, 1200);
  }
  auto* response = new PdfResponse(path, page, target);
  m_pool.start(response);
  return response;
}
