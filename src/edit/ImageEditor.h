#pragma once

#include <QImage>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>

// Quick, non-destructive image corrections.
//
// Crop, rotate in quarter turns, flip and resize, written as a new file beside
// the original. The original is opened read-only and never modified: a copy is
// saved with a numbered name so repeat corrections do not clobber each other,
// exactly like the matte composer.
//
// Order is fixed and matters, because the preview and the writer have to agree:
//
//   1. EXIF orientation is baked into the pixels on read (autoTransform).
//   2. The user's quarter turns are applied.
//   3. Horizontal and vertical flips are applied.
//   4. The crop rectangle is taken, in the rotated-and-flipped frame the
//      preview showed.
//   5. The result is resized to the requested edge, preserving aspect ratio.
//
// Metadata policy: the ICC color profile travels with the pixels; other EXIF
// tags (including the orientation tag, now baked in) are not copied to the
// copy. That is deliberate: a rotated copy that still claimed the old
// orientation would display sideways everywhere else.
class ImageEditor final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
  // An upper bound on the output, so a mistyped resize cannot ask for an
  // allocation that takes the machine down. Preserving aspect means the real
  // result is usually well under this.
  static constexpr qint64 kMaxOutputPixels = 40'000'000;

  struct Transform {
    int quarterTurns = 0;
    bool flipHorizontal = false;
    bool flipVertical = false;
    // A fine straighten angle in degrees, applied after the quarter turns and
    // flips. The image is scaled to fill the frame so no empty corners show,
    // and cropped back to the original size.
    qreal straightenDegrees = 0.0;
    // Normalized (0..1) fractions of the rotated-and-flipped frame, matching
    // what the editor overlay shows. A non-positive width or height means the
    // whole frame.
    qreal cropX = 0.0;
    qreal cropY = 0.0;
    qreal cropW = 1.0;
    qreal cropH = 1.0;
    // Output edge lengths. Zero means "derive from the other, keeping aspect";
    // both zero means no resize.
    int targetWidth = 0;
    int targetHeight = 0;
  };

  explicit ImageEditor(QObject* parent = nullptr);

  [[nodiscard]] bool busy() const { return m_busy; }

  // The post-EXIF-orientation pixel size, so the editor can label output sizes
  // and offer sensible presets. Reads only the header plus the orientation tag.
  Q_INVOKABLE QSize orientedSize(const QString& path) const;

  // Writes "<stem>-edited.<ext>" beside the original, asynchronously. Crop
  // values are fractions (0..1) of the rotated-and-flipped frame; width or
  // height of zero means the full frame. Emits saved() or failed().
  Q_INVOKABLE void saveCopy(const QString& path, int quarterTurns, bool flipHorizontal,
                            bool flipVertical, qreal straightenDegrees, qreal cropX, qreal cropY,
                            qreal cropWidth, qreal cropHeight, int targetWidth, int targetHeight);

  // Copies the selected region (same pipeline, no resize) to the clipboard.
  // Emits copied() or failed(). Nothing is written to disk.
  Q_INVOKABLE void copyRegion(const QString& path, int quarterTurns, bool flipHorizontal,
                              bool flipVertical, qreal straightenDegrees, qreal cropX, qreal cropY,
                              qreal cropWidth, qreal cropHeight);

  // The pure pipeline, shared with the preview provider and unit tests.
  [[nodiscard]] static QImage apply(const QImage& source, const Transform& transform);

  // Where the copy would go, ignoring collisions at the time it is called.
  [[nodiscard]] static QString outputPathFor(const QString& sourcePath);

  // The first "<stem>-edited[-N].<ext>" that is free.
  [[nodiscard]] static QString availableOutputPath(const QString& sourcePath);

signals:
  void saved(const QString& outputPath);
  void copied();
  void failed(const QString& message);
  void busyChanged();

private:
  bool m_busy = false;
};
