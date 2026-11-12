#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

// Keywords, a star rating and a description that other photo tools keep in
// the file itself (XMP, IPTC-IIM) or in an .xmp sidecar beside it.
//
// Read-only and bounded: Omaroll never writes XMP or IPTC. Every reader here
// is a pure function of the bytes it is given, caps what it keeps, and refuses
// anything that looks like an XML entity attack, so a hostile file can only
// produce nothing. The readers run on the metadata index's worker threads.
namespace EmbeddedMetadata {

// Per-file limits. Keywords go through TagNames::normalized, which also caps
// each name.
constexpr int kMaximumKeywords = 200;
constexpr int kMaximumKeywordLength = 120;
// Captions are stored at most this long, as local captions are.
constexpr int kMaximumDescriptionLength = 500;
// One XMP packet, sidecar or embedded.
constexpr qint64 kMaximumPacketBytes = 1024 * 1024;
constexpr int kMaximumXmlDepth = 64;

struct Fields {
  // Tag names, normalized ("Animals/Birds/kestrel"), each once ignoring case,
  // in the order the file lists them.
  QStringList keywords;
  // 1 to 5, or 0 when the file states none (or 0, or -1 for rejected).
  int rating = 0;
  QString description;
  // An XMP packet was found and parsed (even an empty one). IPTC keywords are
  // only used when no XMP was found anywhere.
  bool hasXmp = false;

  [[nodiscard]] bool isEmpty() const {
    return keywords.isEmpty() && rating == 0 && description.isEmpty();
  }
  bool operator==(const Fields& other) const = default;
};

// One XMP packet: an x:xmpmeta or rdf:RDF document. Namespaces are matched by
// URI, both attribute and element forms are read, and rdf:Bag, rdf:Seq and
// rdf:Alt are understood.
//   lr:hierarchicalSubject "A|B|C" becomes "A/B/C".
//   digiKam:TagsList "A/B/C" is used as is.
//   A flat dc:subject keyword equal to any segment of a hierarchical keyword
//   in the same packet is dropped (Lightroom writes both).
//   darktable's own "darktable|..." tags are dropped.
//   xmp:Rating 1 to 5 is used, rounded; anything else is no rating.
//   dc:description: the x-default entry, else the first.
// A DOCTYPE, a packet over kMaximumPacketBytes or nesting deeper than
// kMaximumXmlDepth yields an empty result with hasXmp false.
[[nodiscard]] Fields parseXmp(const QByteArray& packet);

// IPTC-IIM records, as stored in Photoshop's 0x0404 resource: keywords from
// 2:25 and the caption from 2:120. Text is UTF-8 when 1:90 says so or when the
// bytes are valid UTF-8, Latin-1 otherwise.
[[nodiscard]] Fields parseIptc(const QByteArray& records);

// What a container holds: its XMP packet and its IPTC records, either empty.
struct Packets {
  QByteArray xmp;
  QByteArray iptc;
};

// The container walkers. Each takes the start of a file (as much as the
// caller read) and never reads past it. JPEG: APP1 XMP (extended XMP is
// ignored) and APP13 Photoshop IRB 0x0404, walking markers up to SOS. PNG:
// iTXt "XML:com.adobe.xmp", compressed or not. WebP: the "XMP " chunk.
// TIFF, DNG and TIFF-based raws: IFD0 tags 700 (XMP) and 33723 (IPTC), both
// byte orders.
[[nodiscard]] Packets jpegPackets(const QByteArray& head);
[[nodiscard]] Packets pngPackets(const QByteArray& head);
[[nodiscard]] Packets webpPackets(const QByteArray& head);
[[nodiscard]] Packets tiffPackets(const QByteArray& head);
// Picks the walker from the leading bytes; empty for anything else.
[[nodiscard]] Packets packets(const QByteArray& head);

// The file's own fields, without a sidecar: XMP, then IPTC. Reads at most
// what the container needs (a JPEG's markers up to SOS, a PNG's or WebP's
// chunk headers, a TIFF's IFD0 and the two values it points to), never more
// than a few MiB. Empty for videos, documents and unreadable files.
[[nodiscard]] Fields readFile(const QString& path);

// Everything for one file, with the sidecar chosen by the scanner (empty for
// none). For each field the first source that has a value wins: the
// sidecar's XMP, then the file's XMP, then its IPTC. IPTC keywords are used
// only when neither has XMP. A video, or a file whose own data can't be
// read, still gets its sidecar's fields.
[[nodiscard]] Fields read(const QString& path, const QString& sidecar);

// The sidecar for |fileName| among the .xmp files in its folder, keyed by
// their lowercased names. "<name>.<ext>.xmp" (darktable) first, then
// "<name>.xmp" (Lightroom, digiKam), but that one only when
// |baseNameShared| is false: for a camera raw, no other raw shares the base
// name; for anything else, no other media file does. So a RAW+JPEG pair's
// JPEG never takes the raw's sidecar, and two raws never share one. Names
// match ignoring case. Returns the sidecar's actual file name, or empty.
[[nodiscard]] QString sidecarName(const QString& fileName, bool raw, bool baseNameShared,
                                  const QHash<QString, QString>& sidecarsByLowerName);

// The same for one file outside a scan (a file opened in the viewer): lists
// its folder once. Returns the sidecar's absolute path, or empty.
[[nodiscard]] QString findSidecar(const QString& path);

} // namespace EmbeddedMetadata
