#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

#include <atomic>
#include <memory>

// Camera raws for the library and the viewer, off the GUI thread.
//
// Ids come from CameraRaw::previewUrl() and fullUrl():
//   image://raw/preview~<version>/<encoded path>  the embedded preview, upright
//   image://raw/full~<version>/<encoded path>     the full demosaic
// A sourceSize on the Image scales the result down to fit it.
class RawImageProvider final : public QQuickAsyncImageProvider {
public:
  static constexpr const char* kProviderId = "raw";

  RawImageProvider();
  ~RawImageProvider() override;
  void shutdown();

  QQuickImageResponse* requestImageResponse(const QString& id,
                                            const QSize& requestedSize) override;

private:
  // Previews and full decodes in separate pools, so a two-second demosaic
  // never holds up the next picture's preview.
  QThreadPool m_previews;
  QThreadPool m_full;
  std::shared_ptr<std::atomic_bool> m_stopping = std::make_shared<std::atomic_bool>(false);
};
