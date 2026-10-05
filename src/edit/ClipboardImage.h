#pragma once

#include <QImage>

namespace ClipboardImage {

// Puts an image on the Wayland clipboard through wl-copy, which outlives
// omaroll, and falls back to the Qt clipboard elsewhere. Returns false when the persistent offer fails.
// Qt fallback is restricted to other platforms and runs on the GUI thread.
[[nodiscard]] bool offer(const QImage& image);

} // namespace ClipboardImage
