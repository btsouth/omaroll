#pragma once

#include <QColor>
#include <QImage>

// Finds the colour a capture is "about", so a matte can be derived from the
// picture rather than picked from a fixed palette.
//
// Weighted by saturation, so a screenshot that is mostly grey chrome with one
// vivid accent takes its matte from the accent, which is what a person would
// pick by eye. Deterministic: the same file always yields the same hue, so the
// same six mattes are offered every time.
namespace HueExtractor {

// Degrees, 0-359. Returns -1 when the image has no meaningful colour, which is
// common for terminal and editor screenshots.
[[nodiscard]] int dominantHue(const QImage& image);

// The dominant hue as a saturated, mid-lightness colour, or a settled indigo
// when the source is effectively greyscale.
[[nodiscard]] QColor dominantColor(const QImage& image);

} // namespace HueExtractor
