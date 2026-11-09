#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Puts a picture on the Wayland clipboard in more than one type, which wl-copy
// cannot do: the file's own bytes for apps that take them, and PNG beside a
// JPEG, since Omarchy's clipboard history keeps only image/png and text. Other
// still formats (WebP, HEIC, AVIF, JPEG XL, GIF and the rest Qt reads) go on
// as a file reference, text/uri-list, with a PNG of the picture, or of an
// animation's first frame, beside it.
//
// It runs as `omaroll --serve-clipboard <type> <file>`, the type being the
// file's own or kUriList, handled before any Qt
// application exists. It reads the file, takes the selection through the
// ext-data-control protocol, and then, as wl-copy does, leaves a background
// copy of itself serving pastes until something else is copied. The PNG is
// encoded there the first time something asks for it, never in the app.
// That copy works from /, keeps only its Wayland connection open, and stops
// its paste writers when it ends.
namespace ClipboardServe {

inline constexpr char kOption[] = "--serve-clipboard";
// As the type: offer the file's URI rather than its bytes, plus the PNG.
inline constexpr char kUriList[] = "text/uri-list";
// Offered beside kUriList with the same URI, as wl-copy does for a text type.
inline constexpr const char* kUriTextAliases[] = {"text/plain", "text/plain;charset=utf-8", "TEXT",
                                                  "STRING", "UTF8_STRING"};
// No Wayland connection, or a compositor without ext-data-control: the
// caller falls back to wl-copy with the file's own type.
inline constexpr int kUnavailable = 3;
// Something else took the clipboard while this copy was being set up. The
// caller must not fall back, or the older picture would replace it.
inline constexpr int kSuperseded = 4;

// Past this many pixels a JPEG goes on as itself only: a 24 megapixel PNG
// takes about 1.4 s, and clipboard history gives up after 2 s.
inline constexpr qint64 kMaxPngPixels = 24'000'000;

// Whether a picture of this type can get a PNG beside it at all: any still
// or animated format Qt reads, except PNG itself and SVG.
[[nodiscard]] bool mayOfferPng(const QString& sourceType);

// The types offered, the served type first, then kUriTextAliases for a file
// reference. PNG is added last for a picture whose
// header says it decodes within Qt's allocation limit and has at most
// maxPixels, unless the served type is already PNG.
[[nodiscard]] QStringList offeredTypes(const QString& servedType, const QByteArray& bytes,
                                       qint64 maxPixels = kMaxPngPixels);

// The text/uri-list a paste of kUriList gets: the file's URI and CRLF, as
// the app's own file copies write it.
[[nodiscard]] QByteArray uriList(const QString& path);

// A PNG of the picture as shown (EXIF orientation applied), or empty when it
// cannot be read. Light compression: a clipboard history reading it gives up
// after two seconds.
// |sourceType| names the picture's format; empty, its bytes are read for it.
// Either way only a format Omaroll lists is decoded.
[[nodiscard]] QByteArray encodePng(const QByteArray& source, const QString& sourceType = QString());

// The whole helper: argv is {program, kOption, type, file}. Returns the exit
// code for the caller that waits on it; the background copy never returns.
int run(int argc, char** argv);

} // namespace ClipboardServe
