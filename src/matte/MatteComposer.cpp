#include "matte/MatteComposer.h"

#include "matte/HueExtractor.h"

#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <cmath>

namespace {

QColor shifted(const QColor& base, int hueDelta, int saturation, int value) {
  int hue = base.hsvHue();
  if (hue < 0) {
    hue = 238;
  }
  hue = (hue + hueDelta + 360) % 360;
  return QColor::fromHsv(hue, qBound(0, saturation, 255), qBound(0, value, 255));
}

// A soft radial blob, used to build the Aurora mesh.
void paintBlob(QPainter& painter, const QPointF& center, qreal radius, const QColor& color) {
  QRadialGradient gradient(center, radius);
  QColor inner = color;
  inner.setAlphaF(0.55f);
  QColor outer = color;
  outer.setAlphaF(0.0f);
  gradient.setColorAt(0.0, inner);
  gradient.setColorAt(1.0, outer);
  painter.setBrush(gradient);
  painter.setPen(Qt::NoPen);
  painter.drawEllipse(center, radius, radius);
}

} // namespace

MatteComposer::MatteComposer(QObject* parent) : QObject(parent) {}

QStringList MatteComposer::matteNames() const {
  return {QStringLiteral("Adaptive"), QStringLiteral("Deep"),  QStringLiteral("Aurora"),
          QStringLiteral("Slate"),    QStringLiteral("Paper"), QStringLiteral("Pop"),
          QStringLiteral("None")};
}

QStringList MatteComposer::aspectNames() const {
  return {QStringLiteral("Original"), QStringLiteral("1:1"), QStringLiteral("16:9"),
          QStringLiteral("Social")};
}

QImage MatteComposer::paintBackground(const QSize& size, Matte matte, const QColor& seed) {
  QImage background(size, QImage::Format_ARGB32_Premultiplied);
  background.fill(Qt::transparent);

  QPainter painter(&background);
  painter.setRenderHint(QPainter::Antialiasing);

  const QRectF area(QPointF(0, 0), QSizeF(size));

  switch (matte) {
  case Adaptive: {
    QLinearGradient gradient(area.topLeft(), area.bottomRight());
    gradient.setColorAt(0.0, shifted(seed, -12, 60, 250));
    gradient.setColorAt(0.55, shifted(seed, 0, 85, 232));
    gradient.setColorAt(1.0, shifted(seed, 14, 110, 214));
    painter.fillRect(area, gradient);
    break;
  }
  case Deep: {
    QLinearGradient gradient(area.topLeft(), area.bottomRight());
    gradient.setColorAt(0.0, shifted(seed, -10, 150, 74));
    gradient.setColorAt(0.55, shifted(seed, 4, 175, 52));
    gradient.setColorAt(1.0, shifted(seed, 18, 190, 36));
    painter.fillRect(area, gradient);
    break;
  }
  case Aurora: {
    painter.fillRect(area, shifted(seed, 0, 170, 40));
    const qreal unit = std::max(area.width(), area.height());
    paintBlob(painter, QPointF(area.width() * 0.22, area.height() * 0.24), unit * 0.46,
              shifted(seed, -35, 200, 190));
    paintBlob(painter, QPointF(area.width() * 0.82, area.height() * 0.32), unit * 0.38,
              shifted(seed, 30, 205, 170));
    paintBlob(painter, QPointF(area.width() * 0.58, area.height() * 0.86), unit * 0.42,
              shifted(seed, 70, 190, 150));
    break;
  }
  case Slate: {
    QLinearGradient gradient(area.topLeft(), area.bottomLeft());
    gradient.setColorAt(0.0, QColor(46, 49, 54));
    gradient.setColorAt(1.0, QColor(28, 30, 34));
    painter.fillRect(area, gradient);
    break;
  }
  case Paper: {
    QLinearGradient gradient(area.topLeft(), area.bottomLeft());
    gradient.setColorAt(0.0, QColor(246, 245, 242));
    gradient.setColorAt(1.0, QColor(228, 226, 220));
    painter.fillRect(area, gradient);
    break;
  }
  case Pop: {
    QLinearGradient gradient(area.topLeft(), area.topRight());
    gradient.setColorAt(0.0, shifted(seed, 180, 150, 225));
    gradient.setColorAt(1.0, shifted(seed, 150, 185, 190));
    painter.fillRect(area, gradient);
    break;
  }
  case None:
  case MatteCount:
    break;
  }

  return background;
}

QSize MatteComposer::canvasFor(const QSize& content, Aspect aspect, int padding) {
  const QSize padded(content.width() + padding * 2, content.height() + padding * 2);

  qreal ratio = 0.0;
  switch (aspect) {
  case Square:
    ratio = 1.0;
    break;
  case Wide:
    ratio = 16.0 / 9.0;
    break;
  case Social:
    ratio = 1.91;
    break;
  case Original:
  case AspectCount:
    return padded;
  }

  // Grow the short side to reach the ratio; never crop the capture to fit it.
  qreal width = padded.width();
  qreal height = padded.height();
  if (width / height < ratio) {
    width = height * ratio;
  } else {
    height = width / ratio;
  }

  QSize canvas(qRound(width), qRound(height));

  // A forced aspect on a very tall capture can ask for an enormous canvas, so
  // scale the whole thing down rather than allocating it.
  const qint64 pixels = static_cast<qint64>(canvas.width()) * canvas.height();
  if (pixels > kMaxOutputPixels) {
    const qreal scale = std::sqrt(static_cast<qreal>(kMaxOutputPixels) / pixels);
    canvas = QSize(qMax(1, qRound(canvas.width() * scale)),
                   qMax(1, qRound(canvas.height() * scale)));
  }
  return canvas;
}

QImage MatteComposer::compose(const QImage& source, Matte matte, Aspect aspect,
                              qreal paddingFraction) {
  if (source.isNull()) {
    return {};
  }
  if (matte == None) {
    return source;
  }

  const int padding =
      qRound(std::max(source.width(), source.height()) * qBound(0.0, paddingFraction, 0.35));
  const QSize canvasSize = canvasFor(source.size(), aspect, padding);
  if (canvasSize.isEmpty()) {
    return source;
  }

  const QColor seed = HueExtractor::dominantColor(source);
  QImage canvas = paintBackground(canvasSize, matte, seed);

  // Fit the capture inside the padded area, never upscaling it.
  const QSize maxContent(canvasSize.width() - padding * 2, canvasSize.height() - padding * 2);
  QSize drawn = source.size();
  if (maxContent.width() > 0 && maxContent.height() > 0 &&
      (drawn.width() > maxContent.width() || drawn.height() > maxContent.height())) {
    drawn = drawn.scaled(maxContent, Qt::KeepAspectRatio);
  }

  const QRect target(QPoint((canvasSize.width() - drawn.width()) / 2,
                            (canvasSize.height() - drawn.height()) / 2),
                     drawn);

  QPainter painter(&canvas);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);

  // A soft drop shadow lifts the capture off the matte. Drawn as a few
  // decreasing-alpha rounded rects rather than a blur, which is far cheaper and
  // indistinguishable at this size.
  const qreal radius = std::max(2.0, padding * 0.10);
  for (int step = 8; step >= 1; --step) {
    QColor shadow(0, 0, 0);
    shadow.setAlphaF(0.030f);
    painter.setPen(Qt::NoPen);
    painter.setBrush(shadow);
    const QRectF spread = QRectF(target).adjusted(-step, -step + step * 0.4, step, step * 1.4);
    painter.drawRoundedRect(spread, radius + step, radius + step);
  }

  painter.drawImage(target, source);
  painter.end();

  return canvas;
}

void MatteComposer::composeAndSave(const QString& path, int matte, int aspect,
                                   qreal paddingFraction) {
  const QFileInfo info(path);
  if (!info.exists()) {
    emit failed(QStringLiteral("That file is no longer there"));
    return;
  }

  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QImage source = reader.read();
  if (source.isNull()) {
    emit failed(QStringLiteral("Could not read %1").arg(info.fileName()));
    return;
  }

  const QImage result = compose(source, static_cast<Matte>(matte), static_cast<Aspect>(aspect),
                                paddingFraction);
  if (result.isNull()) {
    emit failed(QStringLiteral("Could not build that matte"));
    return;
  }

  // Never overwrite. The original is untouched and repeat composes get their
  // own numbered files rather than clobbering the last one.
  QString output = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() +
                   QStringLiteral("-matte.png");
  int suffix = 2;
  while (QFileInfo::exists(output)) {
    output = QStringLiteral("%1/%2-matte-%3.png")
                 .arg(info.absolutePath(), info.completeBaseName())
                 .arg(suffix++);
  }

  if (!result.save(output, "PNG")) {
    emit failed(QStringLiteral("Could not write %1").arg(QFileInfo(output).fileName()));
    return;
  }

  QGuiApplication::clipboard()->setImage(result);
  emit composed(output);
}
