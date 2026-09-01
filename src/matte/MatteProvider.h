#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

// Live previews for the matte picker, composed off the GUI thread.
//
// Ids arrive as "<matte>.<aspect>.<paddingPercent>/<absolute path>", so a
// single provider serves every combination the picker can show and QML never
// has to hold a composed image itself. Previews are composed from a downscaled
// source: the picker is a set of thumbnails, and composing six full-resolution
// mattes to draw them would be wasted work.
class MatteProvider final : public QQuickAsyncImageProvider {
public:
  static constexpr const char* kProviderId = "matte";

  MatteProvider();

  QQuickImageResponse* requestImageResponse(const QString& id,
                                            const QSize& requestedSize) override;

private:
  QThreadPool m_pool;
};
