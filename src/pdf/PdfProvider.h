#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

class PdfProvider final : public QQuickAsyncImageProvider {
public:
  static constexpr const char* kProviderId = "pdf";

  PdfProvider();
  QQuickImageResponse* requestImageResponse(const QString& id,
                                            const QSize& requestedSize) override;
  void shutdown();

private:
  QThreadPool m_pool;
};
