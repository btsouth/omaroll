#pragma once

#include <QQuickImageProvider>

class QQmlEngine;

// The checkerboard tile behind transparent pictures, in two theme colours:
// "image://checker/<rrggbb>/<rrggbb>/<cell>". Drawn here, pixel by pixel,
// and handed over synchronously, so no image reader thread or SVG renderer
// is involved, and nothing is left decoding when a window closes.
class CheckerProvider final : public QQuickImageProvider {
public:
  static constexpr const char* kProviderId = "checker";
  CheckerProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
  // The two squares, |cell| pixels a side, as one 2x2 tile.
  [[nodiscard]] static QImage tile(const QColor& base, const QColor& second, int cell);
  static void install(QQmlEngine& engine);
};
