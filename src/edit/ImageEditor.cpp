#include "edit/ImageEditor.h"

#include "edit/ClipboardImage.h"
#include "edit/CopyOutput.h"
#include "edit/JpegExif.h"
#include "edit/JpegTransform.h"
#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"
#include "sources/ImageBudget.h"

#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QImageWriter>
#include <QMetaObject>
#include <QTransform>
#include <QtConcurrent>
#include <QtMath>

#include <cmath>

namespace {

// Corrections write a copy in the source's format, and nothing here can write
// a raw. Raw development belongs in darktable or RawTherapee.
const QString kRawRefusal = QStringLiteral("Camera raws are not corrected here");

// The format to write, derived from the source suffix (or an extensionless
// file's header) but only when Qt can actually encode it; otherwise PNG, which
// is always available.
QByteArray writableFormat(const QString& sourcePath) {
  static const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
  QByteArray extension = CaptureScanner::mediaSuffix(sourcePath).toUtf8();
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

// The format |sourcePath| is in, named as writableFormat() names formats.
QByteArray sourceFormat(const QString& sourcePath) {
  const QByteArray extension = CaptureScanner::mediaSuffix(sourcePath).toUtf8();
  if (extension == "jpeg") return "jpg";
  if (extension == "tif") return "tiff";
  return extension;
}

// "<name>-edited.<ext>", or "<name>-edited-2.<ext>" when that is taken. The
// stem is trimmed so a name already near the filesystem limit still fits with
// the suffix appended. |beside| names the folder and stem when it is not the
// file itself: a link, whose copy belongs where the link is.
QString composedOutputPath(const QString& sourcePath, bool mustBeFree, const QString& beside = {}) {
  const QFileInfo info(beside.isEmpty() ? sourcePath : beside);
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

// The start of a file, where JPEG keeps its metadata segments.
QByteArray readHead(const QString& path, qint64 bytes = 4 * 1024 * 1024) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.read(bytes) : QByteArray();
}

// A WebP whose picture is VP8L, the lossless codec, rather than VP8.
bool isLosslessWebp(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  const QByteArray header = file.read(12);
  if (header.size() < 12 || !header.startsWith("RIFF") || header.mid(8, 4) != "WEBP") return false;
  for (int chunk = 0; chunk < 32; ++chunk) {
    const QByteArray head = file.read(8);
    if (head.size() < 8) return false;
    const QByteArray kind = head.left(4);
    if (kind == "VP8L") return true;
    if (kind == "VP8 ") return false;
    const quint32 size = quint32(quint8(head[4])) | (quint32(quint8(head[5])) << 8) |
                         (quint32(quint8(head[6])) << 16) | (quint32(quint8(head[7])) << 24);
    if (!file.seek(file.pos() + size + (size & 1))) return false;
  }
  return false;
}

// Whether a picture that is not a JPEG carries EXIF, which the encoders here
// cannot write back: an "Exif" header or item (TIFF-in-JPEG style, HEIF and
// AVIF), PNG's eXIf chunk or WebP's EXIF chunk.
bool hasForeignExif(const QString& path) {
  const QByteArray head = readHead(path, 256 * 1024);
  return head.contains(QByteArrayLiteral("Exif")) || head.contains(QByteArrayLiteral("eXIf")) ||
         (head.startsWith("RIFF") && head.contains(QByteArrayLiteral("EXIF")));
}

QImage readOriented(const QString& path) {
  QImageReader reader(path);
  reader.setAutoTransform(true);
  return reader.read();
}

// The encoder writes only our private file; publication never replaces a name.
QString writeImage(const QImage& image, const QString& preferred, const QByteArray& format,
                   int quality = 92) {
  CopyOutput output(preferred);
  if (!output.device()) return {};
  QImageWriter writer(output.device(), format);
  if (format == "jpg") writer.setQuality(quality);
  if (!writer.write(image)) return {};
  return output.publish();
}

// A corrected copy, encoded in memory so the original's metadata can be
// inserted into a JPEG, then published like any other copy.
QString writeCopy(const QImage& image, const QString& preferred, const QByteArray& format,
                  const QString& source, const QByteArray& metadata) {
  QByteArray encoded;
  QBuffer buffer(&encoded);
  if (!buffer.open(QIODevice::WriteOnly)) return {};
  QImageWriter writer(&buffer, format);
  // A lossless WebP stays lossless (Qt's WebP writer takes 100 as lossless);
  // the lossy formats keep the quality corrections have always used.
  if (format == "webp") writer.setQuality(isLosslessWebp(source) ? 100 : 92);
  else if (format == "jpg" || format == "avif") writer.setQuality(92);
  if (!writer.write(image)) return {};
  buffer.close();
  if (format == "jpg" && !metadata.isEmpty()) {
    encoded = JpegExif::withSegments(encoded, metadata);
    JpegExif::normalize(encoded, image.size());
  }
  CopyOutput output(preferred);
  if (!output.device() || output.device()->write(encoded) != encoded.size()) return {};
  output.matchPermissions(source);
  return output.publish();
}

struct SaveResult {
  QString output;
  QString error;
  // What the copy is not, that a reader would assume it is: re-encoded rather
  // than turned losslessly, in another format, or without the metadata.
  // Empty when there is nothing to say.
  QString note;
};

SaveResult saveTransform(const QString& source, const ImageEditor::Transform& transform,
                         const QString& beside = {}) {
  if (CameraRaw::isRawFile(source)) return {{}, kRawRefusal, {}};
  QString error;
  QString output;
  QStringList notes;
  const QByteArray format = writableFormat(source);
  const QByteArray original = sourceFormat(source);
  if (!original.isEmpty() && original != format) {
    notes << QStringLiteral("Saved as %1, since %2 files cannot be written here")
                 .arg(QString::fromLatin1(format).toUpper(), QString::fromUtf8(original).toUpper());
  }
  const QString preferred = composedOutputPath(source, false, beside);

  // A quarter turn or flip with no crop and no resize on a JPEG is done
  // losslessly by jpegtran: the pixels are never decoded or re-encoded. An
  // EXIF orientation tag is folded into the same single transform, and the
  // copy's tag then says upright, so the rest of its metadata stays as it is.
  const bool fullFrame = transform.cropX == 0.0 && transform.cropY == 0.0 &&
                         transform.cropW == 1.0 && transform.cropH == 1.0;
  const bool noResize = transform.targetWidth <= 0 && transform.targetHeight <= 0;
  const bool turned = (((transform.quarterTurns % 4) + 4) % 4) != 0 ||
                      transform.flipHorizontal || transform.flipVertical;
  if (format == QByteArrayLiteral("jpg") && turned && qFuzzyIsNull(transform.straightenDegrees) &&
      fullFrame && noResize) {
    QImageReader probe(source);
    const JpegTransform::Symmetry symmetry =
        JpegTransform::compose(probe.transformation(), transform.quarterTurns,
                               transform.flipHorizontal, transform.flipVertical);
    const QSize stored = probe.size();
    const QSize upright = symmetry.quarterTurns % 2 ? stored.transposed() : stored;
    CopyOutput candidate(preferred);
    bool written = false;
    if (!candidate.device()) {
      // Left to the encoding path, which reports the folder.
    } else if (symmetry.identity()) {
      // The tag already undoes the turn: the same bytes, now marked upright.
      QFile file(source);
      written = file.open(QIODevice::ReadOnly) &&
                candidate.device()->write(file.readAll()) == file.size();
    } else if (!JpegTransform::available()) {
      notes << QStringLiteral("Re-encoded, since jpegtran is not installed");
    } else {
      switch (JpegTransform::apply(source, candidate.encoderPath(), symmetry.quarterTurns,
                                   symmetry.flipHorizontal, false)) {
      case JpegTransform::Result::Done:
        written = true;
        break;
      case JpegTransform::Result::NotPerfect:
        notes << QStringLiteral("Re-encoded, since its size cannot be turned losslessly");
        break;
      case JpegTransform::Result::Failed:
        notes << QStringLiteral("Re-encoded, since jpegtran could not turn it");
        break;
      }
    }
    if (written) {
      QFile* device = candidate.device();
      QByteArray bytes = device->seek(0) ? device->readAll() : QByteArray();
      if (JpegExif::normalize(bytes, upright) && (!device->seek(0) || device->write(bytes) != bytes.size())) {
        error = QStringLiteral("Could not safely save a copy beside %1").arg(QFileInfo(source).fileName());
      } else {
        // After jpegtran has written: a read-only mode would stop it opening the copy.
        candidate.matchPermissions(source);
        output = candidate.publish();
        if (output.isEmpty()) error = QStringLiteral("Could not safely save a copy beside %1")
                                         .arg(QFileInfo(source).fileName());
      }
    }
  }

  if (output.isEmpty() && error.isEmpty()) {
    const QImage image = readOriented(source);
    if (image.isNull()) {
      error = ImageBudget::decodeRefusal(ImageBudget::estimate(source));
      if (error.isEmpty()) error = QStringLiteral("Could not read %1").arg(QFileInfo(source).fileName());
    } else {
      const QImage result = ImageEditor::apply(image, transform);
      if (result.isNull()) {
        error = QStringLiteral("That correction left nothing to save");
      } else {
        // The date taken, camera and place come along; the orientation is now
        // in the pixels.
        QByteArray metadata;
        if (format == "jpg") metadata = JpegExif::metadataSegments(readHead(source));
        else if (hasForeignExif(source)) notes << QStringLiteral("Its EXIF metadata was not copied");
        output = writeCopy(result, preferred, format, source, metadata);
        if (output.isEmpty()) {
          error = QStringLiteral("Could not safely save a copy beside %1; check its folder")
                      .arg(QFileInfo(source).fileName());
        }
      }
    }
  }

  return {output, error, error.isEmpty() ? notes.join(QStringLiteral(". ")) : QString()};
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

QString ImageEditor::previewProblem(const QString& path) const {
  if (CameraRaw::isRawFile(path)) return {};
  return ImageBudget::previewRefusal(ImageBudget::estimate(path));
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
                           qreal cropWidth, qreal cropHeight, int targetWidth, int targetHeight, int jobId) {
  if (m_busy) {
    emit failed(QStringLiteral("Still saving the last correction"), jobId);
    return;
  }
  const QFileInfo info(path);
  if (!info.exists()) {
    emit failed(QStringLiteral("That file is no longer there"), jobId);
    return;
  }
  if (CameraRaw::isRawFile(path)) {
    emit failed(kRawRefusal, jobId);
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
                           targetH, jobId] {
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
    const auto [output, error, note] = saveTransform(source, transform);
    (void)note;

    QMetaObject::invokeMethod(
        this,
        [this, output, error, jobId] {
          m_busy = false;
          emit busyChanged();
          if (!error.isEmpty()) {
            emit failed(error, jobId);
          } else {
            emit saved(output, jobId);
          }
        },
        Qt::QueuedConnection);
  });
}

void ImageEditor::saveViewCopy(const QString& path, int quarterTurns, bool flipHorizontal,
                               bool flipVertical, int jobId, const QString& entryPath) {
  if (m_busy) {
    emit failed(QStringLiteral("Still saving the last copy"), jobId);
    return;
  }
  if (!QFileInfo(path).isFile()) {
    emit failed(QStringLiteral("That file is no longer there"), jobId);
    return;
  }
  if (CameraRaw::isRawFile(path)) {
    emit failed(kRawRefusal, jobId);
    return;
  }
  Transform transform;
  transform.quarterTurns = quarterTurns;
  transform.flipHorizontal = flipHorizontal;
  transform.flipVertical = flipVertical;
  if ((((quarterTurns % 4) + 4) % 4) == 0 && flipHorizontal == flipVertical &&
      !flipHorizontal) {
    emit failed(QStringLiteral("The picture is not turned or flipped"), jobId);
    return;
  }

  m_busy = true;
  emit busyChanged();
  const QString source = path;
  const QString beside = entryPath == path ? QString() : entryPath;
  (void)QtConcurrent::run([this, source, beside, transform, jobId] {
    const auto [output, error, note] = saveTransform(source, transform, beside);
    QMetaObject::invokeMethod(
        this,
        [this, output, error, note, jobId] {
          m_busy = false;
          emit busyChanged();
          if (!error.isEmpty()) {
            emit failed(error, jobId);
          } else {
            emit viewCopySaved(output, note, jobId);
          }
        },
        Qt::QueuedConnection);
  });
}

void ImageEditor::copyRegion(const QString& path, int quarterTurns, bool flipHorizontal,
                             bool flipVertical, qreal straightenDegrees, qreal cropX, qreal cropY,
                             qreal cropWidth, qreal cropHeight, int jobId) {
  if (m_busy) {
    emit failed(QStringLiteral("Still working on the last correction"), jobId);
    return;
  }
  const QFileInfo info(path);
  if (!info.exists()) {
    emit failed(QStringLiteral("That file is no longer there"), jobId);
    return;
  }
  if (CameraRaw::isRawFile(path)) {
    emit failed(kRawRefusal, jobId);
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

  (void)QtConcurrent::run([this, source, turns, flipH, flipV, straighten, cx, cy, cw, ch, jobId] {
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
        [this, error, jobId] {
          m_busy = false;
          emit busyChanged();
          if (!error.isEmpty()) {
            emit failed(error, jobId);
          } else {
            emit copied(jobId);
          }
        },
        Qt::QueuedConnection);
  });
}

void ImageEditor::saveCopies(const QStringList& paths, int quarterTurns, bool flipHorizontal,
                             bool flipVertical, int targetWidth, int targetHeight, int jobId) {
  if (m_busy) {
    emit failed(QStringLiteral("Still working on the last correction"), jobId);
    return;
  }
  if (paths.isEmpty()) {
    emit batchFinished(0, 0, {}, jobId);
    return;
  }

  m_busy = true;
  emit busyChanged();

  const QStringList sources = paths;
  const int turns = quarterTurns;
  const bool flipH = flipHorizontal;
  const bool flipV = flipVertical;
  const int targetW = targetWidth;
  const int targetH = targetHeight;

  (void)QtConcurrent::run([this, sources, turns, flipH, flipV, targetW, targetH, jobId] {
    int succeeded = 0;
    int failedCount = 0;
    int done = 0;
    QVariantList results;
    const int total = static_cast<int>(sources.size());
    Transform transform;
    transform.quarterTurns = turns;
    transform.flipHorizontal = flipH;
    transform.flipVertical = flipV;
    transform.targetWidth = targetW;
    transform.targetHeight = targetH;
    for (const QString& source : sources) {
      const auto [output, error, note] = saveTransform(source, transform);
      (void)note;
      if (error.isEmpty()) ++succeeded;
      else ++failedCount;
      results.append(QVariantMap{{QStringLiteral("source"), source},
                                {QStringLiteral("output"), output},
                                {QStringLiteral("error"), error}});
      ++done;
      QMetaObject::invokeMethod(this, [this, done, total, jobId] {
        emit batchProgress(done, total, jobId);
      }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(
        this,
        [this, succeeded, failedCount, results, jobId] {
          m_busy = false;
          emit busyChanged();
          emit batchFinished(succeeded, failedCount, results, jobId);
        },
        Qt::QueuedConnection);
  });
}

bool ImageEditor::isAnimated(const QString& path) const {
  QImageReader reader(path);
  return reader.supportsAnimation() && reader.imageCount() != 1;
}

bool ImageEditor::needsNativeExport(const QString& source) {
  QImageReader reader(source);
  if (reader.transformation() != QImageIOHandler::TransformationNone) return true;
  // Qt exposes ICC profiles on decoded images. Probe a tiny image in the worker;
  // decoders without scaled reads may still decode the full frame there.
  reader.setScaledSize(QSize(1, 1));
  const QColorSpace space = reader.read().colorSpace();
  return space.isValid() && space != QColorSpace(QColorSpace::SRgb);
}

QVariantMap ImageEditor::exportImage(const QString& source, const QString& preferred,
                                    const QByteArray& format, int edge) {
  if ((format != "jpg" && format != "png") || edge <= 0) {
    return {{QStringLiteral("error"), QStringLiteral("Unsupported picture export")}};
  }
  QImage image = readOriented(source);
  if (image.isNull()) {
    return {{QStringLiteral("error"), QStringLiteral("Could not read %1")
                                          .arg(QFileInfo(source).fileName())}};
  }
  if (image.width() > edge) image = image.scaledToWidth(edge, Qt::SmoothTransformation);
  const QString output = writeImage(image, preferred, format, 85);
  return {{QStringLiteral("output"), output},
          {QStringLiteral("error"),
           output.isEmpty() ? QStringLiteral("Could not safely export %1")
                                  .arg(QFileInfo(source).fileName())
                            : QString()}};
}
