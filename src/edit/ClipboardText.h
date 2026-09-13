#pragma once

#include <QString>

namespace ClipboardText {

// Copies text through wl-copy, which outlives omaroll, and falls back to the
// Qt clipboard elsewhere. Returns false only when neither path is available.
[[nodiscard]] bool offer(const QString& text);

} // namespace ClipboardText
