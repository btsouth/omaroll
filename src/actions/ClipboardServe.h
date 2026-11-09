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
// That copy works from /, keeps only its Wayland connection open, and stops
// its paste writers when it ends.
namespace ClipboardServe {

inline constexpr char kOption[] = "--serve-clipboard";
// No Wayland connection, or a compositor without ext-data-control: the
// caller falls back to wl-copy with the file's own type.
inline constexpr int kUnavailable = 3;
// Something else took the clipboard while this copy was being set up. The
// caller must not fall back, or the older picture would replace it.
inline constexpr int kSuperseded = 4;

// Past this many pixels a JPEG goes on as itself only: a 24 megapixel PNG
// takes about 1.4 s, and clipboard history gives up after 2 s.
inline constexpr qint64 kMaxPngPixels = 24'000'000;

// Whether a picture of this type can get a PNG beside it at all.
[[nodiscard]] bool mayOfferPng(const QString& sourceType);

// The types offered for this picture, its own first. PNG is added only for a
// JPEG whose header says it decodes within Qt's allocation limit and has at
// most maxPixels.
[[nodiscard]] QStringList offeredTypes(const QString& sourceType, const QByteArray& bytes,
                                       qint64 maxPixels = kMaxPngPixels);

// A PNG of the picture as shown (EXIF orientation applied), or empty when it
// cannot be read. Light compression: a clipboard history reading it gives up
// after two seconds.
[[nodiscard]] QByteArray encodePng(const QByteArray& source);

// The whole helper: argv is {program, kOption, type, file}. Returns the exit
// code for the caller that waits on it; the background copy never returns.
int run(int argc, char** argv);

} // namespace ClipboardServe
