#pragma once

#include <QImage>

namespace ClipboardImage {

// Puts an image on the Wayland clipboard through wl-copy, which outlives
// omaroll, and falls back to the Qt clipboard elsewhere. Returns false only
// when neither path is available.
[[nodiscard]] bool offer(const QImage& image);

} // namespace ClipboardImage
