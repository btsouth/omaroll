#include "theme/CheckerProvider.h"

#include <QColor>
#include <QQmlEngine>
#include <QStringList>

#include <algorithm>

QImage CheckerProvider::tile(const QColor& base, const QColor& second, int cell) {
  cell = std::clamp(cell, 1, 64);
  QImage image(cell * 2, cell * 2, QImage::Format_RGB32);
  const QRgb first = base.rgb();
  const QRgb other = second.rgb();
  for (int y = 0; y < image.height(); ++y) {
    auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
    for (int x = 0; x < image.width(); ++x) row[x] = (x < cell) == (y < cell) ? other : first;
  }
  return image;
}

QImage CheckerProvider::requestImage(const QString& id, QSize* size, const QSize&) {
  const QStringList parts = id.split(QLatin1Char('/'));
  const QColor base(QLatin1Char('#') + parts.value(0));
  const QColor second(QLatin1Char('#') + parts.value(1));
  bool okay = false;
  const int cell = parts.value(2).toInt(&okay);
  const QImage image = tile(base.isValid() ? base : QColor(Qt::darkGray),
                            second.isValid() ? second : QColor(Qt::gray), okay ? cell : 8);
  if (size) *size = image.size();
  return image;
}

void CheckerProvider::install(QQmlEngine& engine) {
  engine.addImageProvider(QLatin1String(kProviderId), new CheckerProvider);
}
