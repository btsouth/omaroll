#include "matte/HueExtractor.h"

#include <array>

namespace {

// Sampled at this edge length rather than full size. A hue histogram does not
// get more truthful with more pixels, and this keeps the picker instant.
constexpr int kSampleEdge = 96;

// Below this saturation a pixel says nothing about the image's colour, so it is
// not allowed to vote.
constexpr int kMinSaturation = 40;

// Near-black and near-white pixels carry unreliable hue.
constexpr int kMinValue = 30;
constexpr int kMaxValue = 245;

// The settled fallback for a greyscale source. Reads as deliberate rather than
// absent, which a grey matte on a grey screenshot would not.
const QColor kNeutralFallback = QColor::fromHsv(238, 150, 170);

} // namespace

namespace HueExtractor {

int dominantHue(const QImage& image) {
  if (image.isNull()) {
    return -1;
  }

  const QImage sample =
      image.scaled(kSampleEdge, kSampleEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation)
          .convertToFormat(QImage::Format_RGB32);
  if (sample.isNull()) {
    return -1;
  }

  // 36 buckets of 10 degrees. Finer than that and adjacent shades of the same
  // colour split their vote between neighbouring buckets.
  std::array<qint64, 36> buckets{};
  buckets.fill(0);
  qint64 total = 0;

  for (int y = 0; y < sample.height(); ++y) {
    for (int x = 0; x < sample.width(); ++x) {
      const QColor pixel = sample.pixelColor(x, y);
      const int saturation = pixel.hsvSaturation();
      const int value = pixel.value();
      const int hue = pixel.hsvHue();

      if (hue < 0 || saturation < kMinSaturation || value < kMinValue || value > kMaxValue) {
        continue;
      }

      // Weight by saturation so a small vivid area outvotes a large muted one.
      const qint64 weight = saturation;
      buckets[static_cast<size_t>(hue / 10) % buckets.size()] += weight;
      total += weight;
    }
  }

  if (total == 0) {
    return -1;
  }

  size_t best = 0;
  for (size_t index = 1; index < buckets.size(); ++index) {
    if (buckets[index] > buckets[best]) {
      best = index;
    }
  }

  // A winner that barely beats noise is not a colour identity.
  if (buckets[best] * 6 < total) {
    return -1;
  }

  return static_cast<int>(best) * 10 + 5;
}

QColor dominantColor(const QImage& image) {
  const int hue = dominantHue(image);
  if (hue < 0) {
    return kNeutralFallback;
  }
  return QColor::fromHsv(hue, 165, 175);
}

} // namespace HueExtractor
