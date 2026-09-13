#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

// Live previews for the correction editor, transformed off the GUI thread.
//
// Ids arrive as "<quarterTurns>.<flipH>.<flipV>/<absolute path>". The preview
// shows the frame after EXIF orientation and the user's rotate/flip, but before
// the crop: the crop is drawn as an overlay on top, so dragging it does not
// re-compose the picture. The source is decoded downscaled; an editor preview
// never needs the full-resolution pixels.
class EditProvider final : public QQuickAsyncImageProvider {
public:
  static constexpr const char* kProviderId = "edit";

  EditProvider();
  void shutdown();

  QQuickImageResponse* requestImageResponse(const QString& id,
                                            const QSize& requestedSize) override;

private:
  QThreadPool m_pool;
};
