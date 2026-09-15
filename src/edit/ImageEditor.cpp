#include "edit/ImageEditor.h"

#include "edit/ClipboardImage.h"
#include "edit/JpegTransform.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QImageWriter>
#include <QMetaObject>
#include <QSaveFile>
#include <QTransform>
#include <QtConcurrent>
#include <QtMath>

#include <cmath>

namespace {

// The format to write, derived from the source suffix but only when Qt can
// actually encode it; otherwise PNG, which is always available.
QByteArray writableFormat(const QString& sourcePath) {
  static const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
  QByteArray extension = QFileInfo(sourcePath).suffix().toLower().toUtf8();
  if (extension == "jpeg") {
    extension = "jpg";
  }
  if (supported.contains(extension)) {
    return extension;
  }
  if (extension == "tif" && supported.contains("tiff")) {
    return "tiff";
  }
  return "png";
}

// "<name>-edited.<ext>", or "<name>-edited-2.<ext>" when that is taken. The
// stem is trimmed so a name already near the filesystem limit still fits with
// the suffix appended.
QString composedOutputPath(const QString& sourcePath, bool mustBeFree) {
  const QFileInfo info(sourcePath);
  const QByteArray format = writableFormat(sourcePath);
  QString stem = info.completeBaseName();
  constexpr int kNameMax = 255;
  constexpr int kSuffixRoom = 16; // "-edited-999.<ext>"
  const QByteArray extension = (format == "jpg") ? QByteArrayLiteral("jpg") : format;
  while (stem.toUtf8().size() > kNameMax - kSuffixRoom && !stem.isEmpty()) {
    stem.chop(1);
  }
  const QString directory = info.absolutePath();
  QString output = QStringLiteral("%1/%2-edited.%3").arg(directory, stem, QString::fromUtf8(extension));
  if (!mustBeFree) {
    return output;
  }
  int suffix = 2;
  while (QFileInfo::exists(output)) {
    output = QStringLiteral("%1/%2-edited-%3.%4")
                 .arg(directory, stem)
                 .arg(suffix++)
                 .arg(QString::fromUtf8(extension));
  }
  return output;
}

QImage readOriented(const QString& path) {
  QImageReader reader(path);
  reader.setAutoTransform(true);
  return reader.read();
}

// Writes atomically through QSaveFile, so a failure or a full disk leaves no
// half-written file for the scanner to pick up.
bool writeImage(const QImage& image, const QString& output, const QByteArray& format) {
  QSaveFile file(output);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  QImageWriter writer(&file, format);
  if (format == "jpg") {
    writer.setQuality(92);
  }
  if (!writer.write(image)) {
    file.cancelWriting();
    return false;
  }
  return file.commit();
}

} // namespace

ImageEditor::ImageEditor(QObject* parent) : QObject(parent) {}

QImage ImageEditor::apply(const QImage& source, const Transform& transform) {
  if (source.isNull()) {
    return {};
  }
  const QColorSpace space = source.colorSpace();

  QImage image = source;
  const int turns = ((transform.quarterTurns % 4) + 4) % 4;
  if (turns != 0) {
    // FastTransformation keeps a quarter turn exact: no resampling, no blur.
    image = image.transformed(QTransform().rotate(90.0 * turns), Qt::FastTransformation);
  }
  if (transform.flipHorizontal || transform.flipVertical) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    Qt::Orientations orientations;
    if (transform.flipHorizontal) {
      orientations |= Qt::Horizontal;
    }
    if (transform.flipVertical) {
      orientations |= Qt::Vertical;
    }
    image = image.flipped(orientations);
#else
    image = image.mirrored(transform.flipHorizontal, transform.flipVertical);
#endif
  }
  if (!qFuzzyIsNull(transform.straightenDegrees)) {
    // Rotate about the centre and scale just enough to fill the original
    // frame, then crop the centre back out, so straightening never leaves
    // empty corners. The scale is the larger of the two the coverage needs.
    const qreal radians = qDegreesToRadians(transform.straightenDegrees);
    const qreal c = qAbs(qCos(radians));
    const qreal s = qAbs(qSin(radians));
    const qreal w = image.width();
    const qreal h = image.height();
    const qreal scale = qMax((w * c + h * s) / w, (w * s + h * c) / h);
    QTransform turn;
    turn.translate(w / 2.0, h / 2.0);
    turn.rotate(transform.straightenDegrees);
    turn.scale(scale, scale);
    turn.translate(-w / 2.0, -h / 2.0);
    const QImage turned = image.transformed(turn, Qt::SmoothTransformation);
    const QRect centre((turned.width() - image.width()) / 2,
                       (turned.height() - image.height()) / 2, image.width(), image.height());
    image = turned.copy(centre.intersected(turned.rect()));
  }
  if (transform.cropW > 0.0 && transform.cropH > 0.0) {
    const QRect crop(qRound(transform.cropX * image.width()),
                     qRound(transform.cropY * image.height()), qRound(transform.cropW * image.width()),
                     qRound(transform.cropH * image.height()));
    const QRect bounded = crop.intersected(image.rect());
    if (bounded.isEmpty()) {
      return {};
    }
    image = image.copy(bounded);
  }
  if (transform.targetWidth > 0 || transform.targetHeight > 0) {
    QSize target(transform.targetWidth, transform.targetHeight);
    if (target.width() <= 0) {
      target.setWidth(qMax(1, qRound(qreal(target.height()) * image.width() / image.height())));
    } else if (target.height() <= 0) {
      target.setHeight(qMax(1, qRound(qreal(target.width()) * image.height() / image.width())));
    }
    qint64 pixels = static_cast<qint64>(target.width()) * target.height();
    if (pixels > kMaxOutputPixels) {
      const qreal scale = std::sqrt(static_cast<qreal>(kMaxOutputPixels) / pixels);
      target = QSize(qMax(1, qRound(target.width() * scale)),
                     qMax(1, qRound(target.height() * scale)));
    }
    image = image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }

  if (space.isValid()) {
    image.setColorSpace(space);
  }
  return image;
}

QString ImageEditor::outputPathFor(const QString& sourcePath) {
  return composedOutputPath(sourcePath, false);
}

QString ImageEditor::availableOutputPath(const QString& sourcePath) {
  return composedOutputPath(sourcePath, true);
}

QSize ImageEditor::orientedSize(const QString& path) const {
  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QSize stored = reader.size();
  if (!stored.isValid() || stored.isEmpty()) {
    return {};
  }
  // autoTransform was just set, so a quarter-turn tag means the displayed
  // image is the stored one transposed. Rotate90 and Rotate270 both carry the
  // quarter-turn bit.
  if (reader.transformation() & QImageIOHandler::TransformationRotate90) {
    return stored.transposed();
  }
  return stored;
}

void ImageEditor::saveCopy(const QString& path, int quarterTurns, bool flipHorizontal,
                           bool flipVertical, qreal straightenDegrees, qreal cropX, qreal cropY,
                           qreal cropWidth, qreal cropHeight, int targetWidth, int targetHeight) {
  if (m_busy) {
    emit failed(QStringLiteral("Still saving the last correction"));
    return;
  }
  const QFileInfo info(path);
  if (!info.exists()) {
    emit failed(QStringLiteral("That file is no longer there"));
    return;
  }

  m_busy = true;
  emit busyChanged();

  const QString source = path;
  const int turns = quarterTurns;
  const bool flipH = flipHorizontal;
  const bool flipV = flipVertical;
  const qreal straighten = straightenDegrees;
  const qreal cx = cropX;
  const qreal cy = cropY;
  const qreal cw = cropWidth;
  const qreal ch = cropHeight;
  const int targetW = targetWidth;
  const int targetH = targetHeight;

  (void)QtConcurrent::run([this, source, turns, flipH, flipV, straighten, cx, cy, cw, ch, targetW,
                           targetH] {
    QString error;
    QString output;
    const QByteArray format = writableFormat(source);

    // Reserve a name before writing. availableOutputPath() alone races another
    // instance picking the same free name, and QSaveFile's commit would then
    // replace that file. NewOnly makes the reservation exclusive.
    const auto reserveName = [&source]() -> QString {
      for (int attempt = 0; attempt < 64; ++attempt) {
        const QString candidate = availableOutputPath(source);
        QFile reservation(candidate);
        if (reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
          reservation.close();
          return candidate;
        }
      }
      return {};
    };

    // A quarter turn or flip with no crop and no resize on a plain JPEG is done
    // losslessly by jpegtran: the pixels are never decoded or re-encoded. An
    // image with an EXIF orientation tag is left to the recompressing path,
    // which bakes the tag in correctly.
    const bool fullFrame = cx == 0.0 && cy == 0.0 && cw == 1.0 && ch == 1.0;
    const bool noResize = targetW <= 0 && targetH <= 0;
    const bool turned = (((turns % 4) + 4) % 4) != 0 || flipH || flipV;
    if (format == QByteArrayLiteral("jpg") && turned && qFuzzyIsNull(straighten) && fullFrame &&
        noResize && JpegTransform::available()) {
      QImageReader probe(source);
      if (probe.transformation() == QImageIOHandler::TransformationNone) {
        const QString candidate = reserveName();
        if (!candidate.isEmpty()) {
          if (JpegTransform::apply(source, candidate, turns, flipH, flipV)) {
            output = candidate;
          } else {
            QFile::remove(candidate);
          }
        }
      }
    }

    if (output.isEmpty() && error.isEmpty()) {
      const QImage image = readOriented(source);
      if (image.isNull()) {
        error = QStringLiteral("Could not read %1").arg(QFileInfo(source).fileName());
      } else {
        Transform transform;
        transform.quarterTurns = turns;
        transform.flipHorizontal = flipH;
        transform.flipVertical = flipV;
        transform.straightenDegrees = straighten;
        transform.cropX = cx;
        transform.cropY = cy;
        transform.cropW = cw;
        transform.cropH = ch;
        transform.targetWidth = targetW;
        transform.targetHeight = targetH;

        const QImage result = apply(image, transform);
        if (result.isNull()) {
          error = QStringLiteral("That correction left nothing to save");
        } else {
          output = reserveName();
          if (output.isEmpty()) {
            error = QStringLiteral("Could not find a free name beside %1")
                        .arg(QFileInfo(source).fileName());
          } else if (!writeImage(result, output, format)) {
            QFile::remove(output);
            error = QFileInfo(source).absoluteDir().exists()
                        ? QStringLiteral("Could not write %1").arg(QFileInfo(output).fileName())
                        : QStringLiteral("The folder for %1 is no longer there")
                              .arg(QFileInfo(source).fileName());
          }
        }
      }
    }

    QMetaObject::invokeMethod(
        this,
        [this, output, error] {
          m_busy = false;
          emit busyChanged();
          if (!error.isEmpty()) {
            emit failed(error);
          } else {
            emit saved(output);
          }
        },
        Qt::QueuedConnection);
  });
}

void ImageEditor::copyRegion(const QString& path, int quarterTurns, bool flipHorizontal,
                             bool flipVertical, qreal straightenDegrees, qreal cropX, qreal cropY,
                             qreal cropWidth, qreal cropHeight) {
  if (m_busy) {
    emit failed(QStringLiteral("Still working on the last correction"));
    return;
  }
  const QFileInfo info(path);
  if (!info.exists()) {
    emit failed(QStringLiteral("That file is no longer there"));
    return;
  }

  m_busy = true;
  emit busyChanged();

  const QString source = path;
  const int turns = quarterTurns;
  const bool flipH = flipHorizontal;
  const bool flipV = flipVertical;
  const qreal straighten = straightenDegrees;
  const qreal cx = cropX;
  const qreal cy = cropY;
  const qreal cw = cropWidth;
  const qreal ch = cropHeight;

  (void)QtConcurrent::run([this, source, turns, flipH, flipV, straighten, cx, cy, cw, ch] {
    QString error;
    QImage image = readOriented(source);
    if (image.isNull()) {
      error = QStringLiteral("Could not read %1").arg(QFileInfo(source).fileName());
    } else {
      Transform transform;
      transform.quarterTurns = turns;
      transform.flipHorizontal = flipH;
      transform.flipVertical = flipV;
      transform.straightenDegrees = straighten;
      transform.cropX = cx;
      transform.cropY = cy;
      transform.cropW = cw;
      transform.cropH = ch;
      const QImage region = apply(image, transform);
      if (region.isNull() || region.width() < 1 || region.height() < 1) {
        error = QStringLiteral("That selection left nothing to copy");
      } else if (!ClipboardImage::offer(region)) {
        error = QStringLiteral("The clipboard is not reachable");
      }
    }

    QMetaObject::invokeMethod(
        this,
        [this, error] {
          m_busy = false;
          emit busyChanged();
          if (!error.isEmpty()) {
            emit failed(error);
          } else {
            emit copied();
          }
        },
        Qt::QueuedConnection);
  });
}
