#pragma once

#include <QByteArray>
#include <QStringList>

// Puts a picture on the Wayland clipboard in more than one type, which wl-copy
// cannot do: the file's own bytes for apps that take them, and PNG beside a
// JPEG, since Omarchy's clipboard history keeps only image/png and text.
//
// It runs as `omaroll --serve-clipboard <type> <file>`, handled before any Qt
// application exists. It reads the file, takes the selection through the
// ext-data-control protocol, and then, as wl-copy does, leaves a background
// copy of itself serving pastes until something else is copied. The PNG is
// encoded there the first time something asks for it, never in the app.
namespace ClipboardServe {

inline constexpr char kOption[] = "--serve-clipboard";
// No Wayland connection, or a compositor without ext-data-control: the
// caller falls back to wl-copy with the file's own type.
inline constexpr int kUnavailable = 3;

// The types offered for a picture of this type, its own first.
[[nodiscard]] QStringList offeredTypes(const QString& sourceType);

// A PNG of the picture as shown (EXIF orientation applied), or empty when it
// cannot be read. Light compression: a clipboard history reading it gives up
// after two seconds.
[[nodiscard]] QByteArray encodePng(const QByteArray& source);

// The whole helper: argv is {program, kOption, type, file}. Returns the exit
// code for the caller that waits on it; the background copy never returns.
int run(int argc, char** argv);

} // namespace ClipboardServe
