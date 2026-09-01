#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

// Serves thumbnails to QML off the GUI thread, so scrolling a large library
// never blocks on a decode.
//
// Image ids arrive as "<devicePixelRatio>|<absolute path>". QML supplies the
// ratio because only it knows which screen the window is on, and that ratio is
// what makes the difference between a crisp tile and a soft one on the
// fractional scale factors Hyprland commonly runs.
class ThumbnailProvider final : public QQuickAsyncImageProvider {
public:
  static constexpr const char* kProviderId = "thumbs";

  ThumbnailProvider();
  void shutdown();

  QQuickImageResponse* requestImageResponse(const QString& id,
                                            const QSize& requestedSize) override;

private:
  QThreadPool m_pool;
};
