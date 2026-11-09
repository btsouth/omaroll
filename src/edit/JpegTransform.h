#pragma once

#include <QImageIOHandler>
#include <QString>

namespace JpegTransform {

// Whether jpegtran (libjpeg-turbo) is on PATH.
[[nodiscard]] bool available();

enum class Result {
  Done,
  // -perfect refused: the size is not a whole number of JPEG blocks, so a
  // lossless turn would drop the partial edge.
  NotPerfect,
  // jpegtran is missing, failed (a damaged file), timed out, or there was
  // nothing to do.
  Failed,
};

// Losslessly applies a pure rotation/flip to a JPEG by transforming the DCT
// coefficients, so the pixels are never re-encoded. Anything but Done means
// callers recompress instead.
[[nodiscard]] Result apply(const QString& source, const QString& output, int quarterTurns,
                           bool flipHorizontal, bool flipVertical);

// A turn and flips as one: |orientation| (what the file's EXIF tag asks for,
// as QImageReader reports it) first, then |quarterTurns| clockwise, then the
// flips. The result is a quarter turn and at most a horizontal flip.
struct Symmetry {
  int quarterTurns = 0;
  bool flipHorizontal = false;
  [[nodiscard]] bool identity() const { return quarterTurns == 0 && !flipHorizontal; }
};
[[nodiscard]] Symmetry compose(QImageIOHandler::Transformations orientation, int quarterTurns,
                               bool flipHorizontal, bool flipVertical);

} // namespace JpegTransform
