#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QSize>

// The metadata a corrected JPEG copy carries over from its original: the
// date taken, the camera, GPS and the rest of EXIF, XMP and IPTC. Every
// reader here is bounds-checked; a damaged segment is left as it is.
namespace JpegExif {

// The pixels are now upright, so the copy's EXIF must not turn them again:
// Orientation becomes 1, the pixel dimension tags become |size| (when
// valid), and the embedded preview, which still shows the old orientation,
// is unlinked. Edits |jpeg| in place without changing its length. Returns
// whether |jpeg| has an EXIF segment at all.
bool normalize(QByteArray& jpeg, const QSize& size);

// When the picture was taken: DateTimeOriginal, else DateTimeDigitized,
// else the IFD0 DateTime, as local time. Invalid when |jpeg| (which may be
// just the start of the file) carries none of them.
QDateTime dateTaken(const QByteArray& jpeg);

// The APP1 (EXIF, XMP), APP13 (IPTC) and comment segments before the first
// scan, ready to insert into another JPEG. Colour (APP2 ICC, APP14 Adobe)
// is left to the encoder, which writes what its own pixels need.
QByteArray metadataSegments(const QByteArray& jpeg);

// |jpeg| with |segments| inserted after its SOI and any JFIF APP0 header.
QByteArray withSegments(const QByteArray& jpeg, const QByteArray& segments);

} // namespace JpegExif
