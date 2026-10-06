#pragma once

#include <QString>

namespace ClipboardText {

// Copies text through wl-copy, which outlives omaroll, and falls back to the
// Qt clipboard elsewhere. Returns false when the persistent offer fails.
// The Qt fallback, used when wl-copy is missing, runs on the GUI thread.
[[nodiscard]] bool offer(const QString& text);

} // namespace ClipboardText
