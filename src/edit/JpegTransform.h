#pragma once

#include <QString>

namespace JpegTransform {

// Whether jpegtran (libjpeg-turbo) is on PATH.
[[nodiscard]] bool available();

// Losslessly applies a pure rotation/flip to a JPEG by transforming the DCT
// coefficients, so the pixels are never re-encoded. Returns false when the
// transform cannot be done losslessly for the whole image (unknown mapping,
// dimensions that are not a whole number of blocks, jpegtran missing, or the
// process failing); callers then recompress instead.
[[nodiscard]] bool apply(const QString& source, const QString& output, int quarterTurns,
                         bool flipHorizontal, bool flipVertical);

} // namespace JpegTransform
