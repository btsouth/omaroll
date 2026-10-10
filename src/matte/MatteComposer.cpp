#include "matte/MatteComposer.h"

#include "matte/HueExtractor.h"
#include "edit/CopyOutput.h"
#include "sources/CameraRaw.h"
#include "sources/ImageBudget.h"

#include <QClipboard>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImageReader>
#include <QImageWriter>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRadialGradient>
#include <QStandardPaths>
#include <QtConcurrent>

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

// Whether any pixel is actually see-through. Most screenshots carry an alpha
// channel that is opaque everywhere, so the channel alone says nothing.
bool hasTransparency(const QImage& image) {
  if (!image.hasAlphaChannel()) {
    return false;
  }
  const QImage sample = image.scaled(64, 64, Qt::KeepAspectRatio).convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < sample.height(); ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
    for (int x = 0; x < sample.width(); ++x) {
      if (qAlpha(line[x]) < 250) {
        return true;
      }
    }
  }
  return false;
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

QSize MatteComposer::canvasFor(const QSize& content, Aspect aspect, int& padding) {
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
    break;
  }

  // Grow the short side to reach the ratio; never crop the capture to fit it.
  qreal width = padded.width();
  qreal height = padded.height();
  if (ratio > 0.0) {
    if (width / height < ratio) {
      width = height * ratio;
    } else {
      height = width / ratio;
    }
  }

  QSize canvas(qRound(width), qRound(height));

  // A forced aspect on a very tall capture can ask for an enormous canvas, so
  // scale the whole thing down rather than allocating it.
  const qint64 pixels = static_cast<qint64>(canvas.width()) * canvas.height();
  if (pixels > kMaxOutputPixels) {
    const qreal scale = std::sqrt(static_cast<qreal>(kMaxOutputPixels) / pixels);
    canvas = QSize(qMax(1, qRound(canvas.width() * scale)),
                   qMax(1, qRound(canvas.height() * scale)));
    // The padding was measured against the unscaled capture. Left alone it
    // would eat the shrunken canvas: a tall screenshot came out as a sliver
    // with more than half the width in matte, and past a point the content
    // area went negative and the capture was cropped.
    padding = qMax(2, qRound(padding * scale));
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

  // A fraction of the longest edge, with a floor so a tiny capture still gets
  // a visible matte rather than a one-pixel frame.
  int padding = std::max(
      24, qRound(std::max(source.width(), source.height()) * qBound(0.0, paddingFraction, 0.35)));
  const QSize canvasSize = canvasFor(source.size(), aspect, padding);
  if (canvasSize.isEmpty()) {
    return source;
  }

  const QImage content = source.colorSpace().isValid()
      ? source.convertedToColorSpace(QColorSpace::SRgb) : source;
  const QColor seed = HueExtractor::dominantColor(content);
  QImage canvas = paintBackground(canvasSize, matte, seed);
  canvas.setColorSpace(QColorSpace::SRgb);

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
  // indistinguishable at this size. Not under a capture with transparent
  // pixels, where the slab would show through as a grey box.
  const qreal radius = std::max(2.0, padding * 0.10);
  for (int step = hasTransparency(source) ? 0 : 8; step >= 1; --step) {
    QColor shadow(0, 0, 0);
    shadow.setAlphaF(0.030f);
    painter.setPen(Qt::NoPen);
    painter.setBrush(shadow);
    const QRectF spread = QRectF(target).adjusted(-step, -step + step * 0.4, step, step * 1.4);
    painter.drawRoundedRect(spread, radius + step, radius + step);
  }

  painter.drawImage(target, content);
  painter.end();

  return canvas;
}

void MatteComposer::composeAndSave(const QString& path, int matte, int aspect,
                                   qreal paddingFraction) {
  if (m_busy) {
    emit failed(QStringLiteral("Still working on the last matte"));
    return;
  }
  if (matte < 0 || matte >= MatteCount || aspect < 0 || aspect >= AspectCount ||
      !std::isfinite(paddingFraction)) {
    emit failed(QStringLiteral("Choose a valid matte and aspect"));
    return;
  }
  m_busy = true;
  emit busyChanged();
  const bool qtFallback = !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
  watch(QtConcurrent::run([path, matte, aspect, paddingFraction, qtFallback] {
    return saveAndCopy(path, static_cast<Matte>(matte), static_cast<Aspect>(aspect), paddingFraction,
                       qtFallback);
  }), false);
}

void MatteComposer::retryCopy() {
  if (m_busy) {
    emit failed(QStringLiteral("Still working on the last matte"));
    return;
  }
  if (m_lastOutputPath.isEmpty()) {
    emit failed(QStringLiteral("There is no saved matte to copy"));
    return;
  }
  m_busy = true;
  emit busyChanged();
  const QString path = m_lastOutputPath;
  const bool qtFallback = !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
  watch(QtConcurrent::run([path, qtFallback] {
    return copySaved(path, QFileInfo(path).isFile(), qtFallback);
  }), true);
}

void MatteComposer::watch(QFuture<Result> future, bool retry) {
  // The watcher and its callback belong to this QObject. Deleting the composer
  // disconnects the callback; workers capture only values and never touch it.
  auto* watcher = new QFutureWatcher<Result>(this);
  connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, retry] {
    Result result = watcher->result();
    watcher->deleteLater();
    if (!result.fallbackImage.isNull()) {
      // QClipboard must be touched on the GUI thread. Decoding and encoding
      // stay in the worker. On Wayland we require the persistent wl-copy offer.
      if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setImage(result.fallbackImage);
        result.copied = true;
      } else {
        result.error = QStringLiteral("Could not copy the matte to the clipboard");
      }
    }
    if (result.saved && !retry) {
      m_lastOutputPath = result.outputPath;
      emit lastOutputPathChanged();
    }
    m_busy = false;
    emit busyChanged();
    if (result.saved && !retry) emit saved(result.outputPath);
    if (result.copied) emit copied(result.outputPath);
    if (!result.error.isEmpty()) {
      emit failed(result.saved
                      ? QStringLiteral("Saved %1, but could not copy it. Retry Copy to try again. %2")
                            .arg(QFileInfo(result.outputPath).fileName(), result.error)
                      : result.error);
    }
    if (result.saved && result.copied && !retry) emit composed(result.outputPath);
    emit finished(result.outputPath, result.saved, result.copied);
  });
  watcher->setFuture(future);
}

MatteComposer::Result MatteComposer::copySaved(const QString& path, bool saved, bool qtFallback) {
  Result result;
  result.outputPath = path;
  result.saved = saved;
  const QFileInfo info(path);
  if (!info.isFile() || !info.isReadable()) {
    result.error = QStringLiteral("The saved matte is no longer readable");
    return result;
  }
  const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
  if (!wlCopy.isEmpty()) {
    // Feed the already encoded file instead of encoding another PNG on the GUI
    // thread. wl-copy forks the persistent offer and its parent exits.
    QProcess process;
    QByteArray errors;
    process.setStandardOutputFile(QProcess::nullDevice());
    connect(&process, &QProcess::readyReadStandardError, &process, [&process, &errors] {
      errors = (errors + process.readAllStandardError()).right(4096);
    });
    process.setStandardInputFile(path);
    process.start(wlCopy, {QStringLiteral("--type"), QStringLiteral("image/png")});
    const bool completed = process.waitForFinished(5000);
    result.copied = completed && process.exitStatus() == QProcess::NormalExit &&
                    process.exitCode() == 0;
    if (!result.copied) {
      if (!completed && process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
      }
      errors = (errors + process.readAllStandardError()).right(4096);
      const QString detail = QString::fromUtf8(errors).trimmed();
      result.error = !detail.isEmpty() ? detail
                     : completed ? QStringLiteral("wl-copy did not finish successfully")
                                 : QStringLiteral("wl-copy could not start or timed out");
    }
    // A failing helper is a real failure, never masked by a transient Qt offer.
    return result;
  }
  if (!qtFallback) {
    result.error = QStringLiteral("Install wl-clipboard to copy images");
    return result;
  }
  QImageReader reader(path);
  result.fallbackImage = reader.read();
  if (result.fallbackImage.isNull()) {
    result.error = QStringLiteral("Could not read the saved matte for copying");
  }
  return result;
}

MatteComposer::Result MatteComposer::saveAndCopy(const QString& path, Matte matte, Aspect aspect,
                                                qreal paddingFraction, bool qtFallback) {
  Result outcome;
  const QFileInfo info(path);
  if (!info.isFile()) {
    outcome.error = QStringLiteral("That file is no longer there");
    return outcome;
  }
  QImageReader reader(path);
  reader.setAutoTransform(true);
  // Match the RAW preview used by the library and the sheet.
  const QImage source = CameraRaw::isRawFile(path) ? CameraRaw::readPreview(path) : reader.read();
  if (source.isNull()) {
    outcome.error = CameraRaw::isRawFile(path) ? QString()
                                               : ImageBudget::decodeRefusal(ImageBudget::estimate(path));
    if (outcome.error.isEmpty()) outcome.error = QStringLiteral("Could not read %1").arg(info.fileName());
    return outcome;
  }
  const QImage image = compose(source, matte, aspect, paddingFraction);
  if (image.isNull()) {
    outcome.error = QStringLiteral("Could not build that matte");
    return outcome;
  }

  QString stem = info.completeBaseName();
  constexpr int kNameMax = 255;
  constexpr int kSuffixRoom = 16;
  while (stem.toUtf8().size() > kNameMax - kSuffixRoom && !stem.isEmpty()) stem.chop(1);
  CopyOutput copy(info.absolutePath() + QLatin1Char('/') + stem + QStringLiteral("-matte.png"));
  if (!copy.device()) {
    outcome.error = QStringLiteral("Could not create a private copy beside %1").arg(info.fileName());
    return outcome;
  }
  QImageWriter writer(copy.device(), "PNG");
  const QString output = writer.write(image) ? copy.publish() : QString();
  if (output.isEmpty()) {
    outcome.error = QStringLiteral("Could not safely save a copy beside %1; check its folder")
                        .arg(info.fileName());
    return outcome;
  }
  return copySaved(output, true, qtFallback);
}
