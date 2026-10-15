#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

#include <atomic>
#include <memory>

// The reduced copy of a picture larger than the screen, for the viewer and
// the library.
//
// Ids come from ImageBudget::reducedUrl():
//   image://large/<edge>~<version>/<encoded path>
// The picture is decoded with its longest side at most <edge>, upright. A
// JPEG decodes at that size directly. Other formats decode whole within Qt's
// allocation limit and are scaled down at once, so only the copy is kept.
//
// Ids from ImageBudget::areaUrl() ask for part of a picture instead:
//   image://large/<x>,<y>,<width>,<height>,<output width>,<output height>~<version>/<path>
// The area is in upright pixels, and is decoded no larger than the output
// size, upright. The viewer shows a zoomed-in very large JPEG this way.
class LargeImageProvider final : public QQuickAsyncImageProvider {
public:
  static constexpr const char* kProviderId = "large";

  LargeImageProvider();
  ~LargeImageProvider() override;
  void shutdown();

  QQuickImageResponse* requestImageResponse(const QString& id,
                                            const QSize& requestedSize) override;

private:
  // Two, so the picture on screen never waits behind a neighbour's copy.
  // Whole decodes of very large pictures still take turns process-wide.
  QThreadPool m_pool;
  std::shared_ptr<std::atomic_bool> m_stopping = std::make_shared<std::atomic_bool>(false);
};
