#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

#include <atomic>
#include <memory>

// The reduced copy of a very large picture, for the viewer and the library.
//
// Ids come from ImageBudget::reducedUrl():
//   image://large/<edge>~<version>/<encoded path>
// The picture is decoded with its longest side at most <edge>, upright. A
// JPEG decodes at that size directly. Other formats decode whole within Qt's
// allocation limit and are scaled down at once, so only the copy is kept.
class LargeImageProvider final : public QQuickAsyncImageProvider {
public:
  static constexpr const char* kProviderId = "large";

  LargeImageProvider();
  ~LargeImageProvider() override;
  void shutdown();

  QQuickImageResponse* requestImageResponse(const QString& id,
                                            const QSize& requestedSize) override;

private:
  // One at a time: two whole decodes of very large pictures at once are what
  // the allocation limit is there to prevent.
  QThreadPool m_pool;
  std::shared_ptr<std::atomic_bool> m_stopping = std::make_shared<std::atomic_bool>(false);
};
