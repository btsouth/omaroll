#pragma once

#include <QColorSpace>
#include <QImage>

// Qt Quick draws stored pixel values as sRGB. A picture tagged with a wider
// profile, such as Display P3 or Adobe RGB, would look washed out, so convert
// it once off the GUI thread. Untagged pictures are taken as sRGB already.
// Image elements loading files do the same with their colorSpace property;
// image providers have to do it themselves.
inline void convertToDisplayColors(QImage& image) {
  const QColorSpace space = image.colorSpace();
  if (image.isNull() || !space.isValid() || space == QColorSpace(QColorSpace::SRgb)) return;
  if (space.colorModel() == QColorSpace::ColorModel::Rgb) {
    image.convertToColorSpace(QColorSpace::SRgb);
  } else {
    // Gray and CMYK profiles need an RGB result to be converted into.
    image = image.convertedToColorSpace(QColorSpace::SRgb, image.hasAlphaChannel()
                                                           ? QImage::Format_ARGB32
                                                           : QImage::Format_RGB32);
  }
}
