#include "library/EmbeddedMetadata.h"

#include "library/TagNames.h"
#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace EmbeddedMetadata {
namespace {

constexpr QLatin1StringView kRdf("http://www.w3.org/1999/02/22-rdf-syntax-ns#");
constexpr QLatin1StringView kDc("http://purl.org/dc/elements/1.1/");
constexpr QLatin1StringView kXmp("http://ns.adobe.com/xap/1.0/");
constexpr QLatin1StringView kLightroom("http://ns.adobe.com/lightroom/1.0/");
constexpr QLatin1StringView kDigiKam("http://www.digikam.org/ns/1.0/");
constexpr QLatin1StringView kXmlNamespace("http://www.w3.org/XML/1998/namespace");

// Markers, chunks and IFD entries a walker looks at before giving up.
constexpr int kMaximumSegments = 4096;
constexpr int kMaximumIfdEntries = 1000;
constexpr int kMaximumResources = 1000;
// qUncompress grows its output until the stream ends, whatever size the
// header claims, and deflate expands at most about 1032 to 1. A compressed
// packet is therefore refused above 32 KiB, which holds well over 100 KiB of
// real XMP and bounds a zlib bomb to about 33 MiB before it is thrown away.
// Capping the output itself would need zlib's own streaming API, a library
// Omaroll does not link; bounded memory is what matters here.
constexpr qsizetype kMaximumCompressedPacket = 32 * 1024;
// All the bytes readFile may read from one file.
constexpr qint64 kFileReadBudget = 3 * kMaximumPacketBytes + 256 * 1024;

const QByteArray kJpegXmpPrefix = QByteArrayLiteral("http://ns.adobe.com/xap/1.0/\0");
const QByteArray kPhotoshopPrefix = QByteArrayLiteral("Photoshop 3.0\0");
const QByteArray kPngSignature = QByteArrayLiteral("\x89PNG\r\n\x1a\n");

// Random access to the start of a file. A read returns fewer bytes than asked
// at the end of the data, and nothing for a negative or overlong request.
class Source {
public:
  virtual ~Source() = default;
  virtual QByteArray read(qint64 offset, qint64 length) = 0;
  // Exactly |length| bytes or nothing.
  QByteArray readExactly(qint64 offset, qint64 length) {
    QByteArray bytes = read(offset, length);
    return bytes.size() == length ? bytes : QByteArray();
  }
};

class BytesSource final : public Source {
public:
  explicit BytesSource(const QByteArray& bytes) : m_bytes(bytes) {}
  QByteArray read(qint64 offset, qint64 length) override {
    if (offset < 0 || length <= 0 || offset >= m_bytes.size()) return {};
    return m_bytes.mid(offset, qMin<qint64>(length, m_bytes.size() - offset));
  }

private:
  const QByteArray& m_bytes;
};

class FileSource final : public Source {
public:
  explicit FileSource(QFile& file) : m_file(file) {}
  QByteArray read(qint64 offset, qint64 length) override {
    if (offset < 0 || length <= 0 || length > m_budget || !m_file.seek(offset)) return {};
    QByteArray bytes = m_file.read(length);
    m_budget -= bytes.size();
    return bytes;
  }

private:
  QFile& m_file;
  qint64 m_budget = kFileReadBudget;
};

// Callers check the bounds first; these check again, so no read can pass them.
quint16 be16(const QByteArray& bytes, qsizetype at) {
  return at >= 0 && at + 2 <= bytes.size() ? qFromBigEndian<quint16>(bytes.constData() + at) : 0;
}
quint32 be32(const QByteArray& bytes, qsizetype at) {
  return at >= 0 && at + 4 <= bytes.size() ? qFromBigEndian<quint32>(bytes.constData() + at) : 0;
}
quint32 le32(const QByteArray& bytes, qsizetype at) {
  return at >= 0 && at + 4 <= bytes.size() ? qFromLittleEndian<quint32>(bytes.constData() + at) : 0;
}

// Photoshop image resources: "8BIM", a 2-byte id, a Pascal name padded to an
// even length, a 4-byte size and the data, padded to even. 0x0404 is IPTC.
QByteArray iptcResource(const QByteArray& resources) {
  qint64 at = 0;
  const qint64 size = resources.size();
  for (int count = 0; count < kMaximumResources && at + 12 <= size; ++count) {
    if (resources.mid(at, 4) != "8BIM") break;
    const quint16 id = be16(resources, at + 4);
    qint64 name = 1 + quint8(resources.at(at + 6));
    name += name & 1;
    const qint64 sizeAt = at + 6 + name;
    if (sizeAt + 4 > size) break;
    const qint64 length = be32(resources, sizeAt);
    const qint64 data = sizeAt + 4;
    if (length > size - data) break;
    if (id == 0x0404) return resources.mid(data, length);
    at = data + length + (length & 1);
  }
  return {};
}

Packets jpegWalk(Source& source) {
  Packets found;
  if (source.readExactly(0, 2) != QByteArrayLiteral("\xFF\xD8")) return found;
  qint64 at = 2;
  for (int count = 0; count < kMaximumSegments; ++count) {
    const QByteArray header = source.read(at, 4);
    if (header.size() < 2 || quint8(header.at(0)) != 0xFF) break;
    const quint8 marker = quint8(header.at(1));
    if (marker == 0xFF) { // fill byte
      ++at;
      continue;
    }
    if (marker == 0xD9 || marker == 0xDA) break;
    if (marker == 0x01 || marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      at += 2;
      continue;
    }
    if (header.size() < 4) break;
    const quint16 length = be16(header, 2);
    if (length < 2) break;
    const qint64 dataLength = length - 2;
    if (marker == 0xE1 && found.xmp.isEmpty()) {
      const QByteArray data = source.readExactly(at + 4, dataLength);
      if (data.isEmpty() && dataLength > 0) break;
      if (data.startsWith(kJpegXmpPrefix)) found.xmp = data.mid(kJpegXmpPrefix.size());
    } else if (marker == 0xED && found.iptc.isEmpty()) {
      const QByteArray data = source.readExactly(at + 4, dataLength);
      if (data.isEmpty() && dataLength > 0) break;
      if (data.startsWith(kPhotoshopPrefix)) found.iptc = iptcResource(data.mid(kPhotoshopPrefix.size()));
    }
    at += 2 + length;
  }
  return found;
}

// zlib data inflated with a bound on what it may expand to.
QByteArray inflated(const QByteArray& compressed) {
  if (compressed.isEmpty() || compressed.size() > kMaximumCompressedPacket) return {};
  QByteArray framed(4, '\0');
  qToBigEndian<quint32>(quint32(kMaximumPacketBytes), framed.data());
  framed += compressed;
  const QByteArray out = qUncompress(framed);
  return out.size() > kMaximumPacketBytes ? QByteArray() : out;
}

// The XMP in an iTXt chunk's data, or nothing when it holds something else.
QByteArray itxtXmp(const QByteArray& data) {
  static const QByteArray keyword = QByteArrayLiteral("XML:com.adobe.xmp");
  if (!data.startsWith(keyword) || data.size() < keyword.size() + 3 || data.at(keyword.size()) != '\0')
    return {};
  qsizetype at = keyword.size() + 1;
  const char compressed = data.at(at);
  const char method = data.at(at + 1);
  at += 2;
  for (int field = 0; field < 2; ++field) { // language, translated keyword
    const qsizetype end = data.indexOf('\0', at);
    if (end < 0) return {};
    at = end + 1;
  }
  const QByteArray text = data.mid(at);
  if (compressed == 0) return text;
  if (compressed == 1 && method == 0) return inflated(text);
  return {};
}

Packets pngWalk(Source& source) {
  Packets found;
  if (source.readExactly(0, 8) != kPngSignature) return found;
  qint64 at = 8;
  for (int count = 0; count < kMaximumSegments; ++count) {
    const QByteArray header = source.readExactly(at, 8);
    if (header.isEmpty()) break;
    const qint64 length = be32(header, 0);
    if (length > 0x7FFFFFFF) break;
    const QByteArray type = header.mid(4, 4);
    if (type == "IEND") break;
    if (type == "iTXt" && length <= kMaximumPacketBytes + 1024) {
      // Only the keyword first, so other text chunks are not read whole.
      const QByteArray start = source.read(at + 8, qMin<qint64>(length, 18));
      if (start.size() == qMin<qint64>(length, 18) && start == QByteArrayLiteral("XML:com.adobe.xmp\0").left(start.size())) {
        const QByteArray data = source.readExactly(at + 8, length);
        if (data.isEmpty()) break;
        found.xmp = itxtXmp(data);
        if (!found.xmp.isEmpty()) break;
      }
    }
    at += 12 + length;
  }
  return found;
}

Packets webpWalk(Source& source) {
  Packets found;
  const QByteArray header = source.readExactly(0, 12);
  if (header.isEmpty() || !header.startsWith("RIFF") || header.mid(8, 4) != "WEBP") return found;
  qint64 at = 12;
  for (int count = 0; count < kMaximumSegments; ++count) {
    const QByteArray chunk = source.readExactly(at, 8);
    if (chunk.isEmpty()) break;
    const qint64 length = le32(chunk, 4);
    if (chunk.startsWith("XMP ")) {
      if (length <= kMaximumPacketBytes) found.xmp = source.readExactly(at + 8, length);
      break;
    }
    at += 8 + length + (length & 1);
  }
  return found;
}

bool tiffMagic(const QByteArray& head) {
  if (head.size() < 8) return false;
  const QByteArray magic = head.left(4);
  return magic == QByteArrayLiteral("II*\0") || magic == QByteArrayLiteral("MM\0*") ||
         magic == "IIRO" || magic == "IIRS" || magic == QByteArrayLiteral("IIU\0") ||
         magic == "MMOR";
}

Packets tiffWalk(Source& source) {
  Packets found;
  const QByteArray header = source.readExactly(0, 8);
  if (!tiffMagic(header)) return found;
  const bool big = header.startsWith("MM");
  const auto u16 = [big](const QByteArray& bytes, qsizetype at) {
    return big ? qFromBigEndian<quint16>(bytes.constData() + at)
               : qFromLittleEndian<quint16>(bytes.constData() + at);
  };
  const auto u32 = [big](const QByteArray& bytes, qsizetype at) {
    return big ? qFromBigEndian<quint32>(bytes.constData() + at)
               : qFromLittleEndian<quint32>(bytes.constData() + at);
  };
  const qint64 first = u32(header, 4);
  const QByteArray countBytes = source.readExactly(first, 2);
  if (first < 8 || countBytes.isEmpty()) return found;
  const int count = u16(countBytes, 0);
  if (count <= 0 || count > kMaximumIfdEntries) return found;
  const QByteArray table = source.readExactly(first + 2, qint64(count) * 12);
  if (table.isEmpty()) return found;
  for (int index = 0; index < count; ++index) {
    const qsizetype at = qsizetype(index) * 12;
    const quint16 tag = u16(table, at);
    if (tag != 700 && tag != 33723) continue;
    QByteArray& target = tag == 700 ? found.xmp : found.iptc;
    if (!target.isEmpty()) continue;
    const quint16 type = u16(table, at + 2);
    // BYTE, ASCII and UNDEFINED are a byte each; Photoshop writes IPTC as LONG.
    int unit = 0;
    if (type == 1 || type == 7 || (tag == 700 && type == 2)) unit = 1;
    else if (tag == 33723 && type == 4) unit = 4;
    if (unit == 0) continue;
    const quint64 length = quint64(u32(table, at + 4)) * quint64(unit);
    if (length == 0 || length > quint64(kMaximumPacketBytes)) continue;
    target = length <= 4 ? table.mid(at + 8, qsizetype(length))
                         : source.readExactly(u32(table, at + 8), qint64(length));
  }
  return found;
}

Packets sniffAndWalk(Source& source) {
  const QByteArray head = source.read(0, 16);
  if (head.startsWith(QByteArrayLiteral("\xFF\xD8\xFF"))) return jpegWalk(source);
  if (head.startsWith(kPngSignature)) return pngWalk(source);
  if (head.size() >= 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") return webpWalk(source);
  if (tiffMagic(head)) return tiffWalk(source);
  return {};
}

bool startsWithDarktable(const QString& keyword, QChar separator) {
  return keyword.section(separator, 0, 0).trimmed().compare(u"darktable", Qt::CaseInsensitive) == 0;
}

// Keeps a keyword once ignoring case, up to the per-file limit.
class KeywordList {
public:
  void add(const QString& name) {
    if (name.isEmpty() || m_names.size() >= kMaximumKeywords) return;
    const QString key = TagNames::folded(name);
    if (m_seen.contains(key)) return;
    m_seen.insert(key);
    m_names.append(name);
  }
  [[nodiscard]] QStringList names() const { return m_names; }

private:
  QStringList m_names;
  QSet<QString> m_seen;
};

// Raw text over the limit is skipped rather than cut, since a cut keyword
// would be a tag nobody wrote.
bool tooLong(const QString& raw) { return raw.size() > kMaximumKeywordLength; }

QStringList xmpKeywords(const QStringList& hierarchical, const QStringList& tagsList, const QStringList& flat) {
  KeywordList keywords;
  QSet<QString> segments;
  const auto nested = [&](const QString& name) {
    for (const QString& segment : name.split(QLatin1Char('/'))) segments.insert(TagNames::folded(segment));
    keywords.add(name);
  };
  // "A|B|C". A slash inside a segment nests too, as it would typed as a tag.
  for (const QString& item : hierarchical) {
    const QString raw = item.trimmed();
    if (tooLong(raw) || startsWithDarktable(raw, QLatin1Char('|'))) continue;
    const QString name = TagNames::normalized(QString(raw).replace(QLatin1Char('|'), QLatin1Char('/')));
    if (!name.isEmpty()) nested(name);
  }
  for (const QString& item : tagsList) {
    const QString raw = item.trimmed();
    if (tooLong(raw) || startsWithDarktable(raw, QLatin1Char('/'))) continue;
    const QString name = TagNames::normalized(raw);
    if (!name.isEmpty()) nested(name);
  }
  for (const QString& item : flat) {
    const QString raw = item.trimmed();
    if (tooLong(raw) || startsWithDarktable(raw, QLatin1Char('|')) || startsWithDarktable(raw, QLatin1Char('/')))
      continue;
    const QString name = TagNames::normalized(raw);
    if (!name.isEmpty() && !segments.contains(TagNames::folded(name))) keywords.add(name);
  }
  return keywords.names();
}

int ratingFrom(const QString& text) {
  bool ok = false;
  const double value = text.trimmed().toDouble(&ok);
  if (!ok || !std::isfinite(value)) return 0;
  const double rounded = std::round(value);
  return rounded >= 1 && rounded <= 5 ? int(rounded) : 0;
}

QString descriptionFrom(const QString& text) { return text.trimmed().left(kMaximumDescriptionLength); }

// Well-formed UTF-8: no stray continuation bytes, overlong forms, surrogates
// or code points past U+10FFFF. (QStringDecoder replaces such bytes without
// reporting an error.)
bool validUtf8(const QByteArray& bytes) {
  const qsizetype size = bytes.size();
  for (qsizetype at = 0; at < size;) {
    const quint8 lead = quint8(bytes.at(at));
    if (lead < 0x80) {
      ++at;
      continue;
    }
    // Continuation bytes after the lead, and the smallest code point that
    // needs that many.
    const int extra = (lead & 0xE0) == 0xC0 ? 1 : (lead & 0xF0) == 0xE0 ? 2 : (lead & 0xF8) == 0xF0 ? 3 : 0;
    if (extra == 0) return false;
    const quint32 minimum = extra == 1 ? 0x80 : extra == 2 ? 0x800 : 0x10000;
    quint32 point = lead & (0x7F >> (extra + 1));
    if (extra > size - 1 - at) return false;
    for (int index = 1; index <= extra; ++index) {
      const quint8 next = quint8(bytes.at(at + index));
      if ((next & 0xC0) != 0x80) return false;
      point = point << 6 | (next & 0x3F);
    }
    if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
    at += 1 + extra;
  }
  return true;
}

} // namespace

Fields parseXmp(const QByteArray& packet) {
  if (packet.isEmpty() || packet.size() > kMaximumPacketBytes) return {};
  // Padding before the packet (an XML declaration must come first) and NULs
  // some writers leave after it.
  qsizetype start = 0;
  while (start < packet.size() && (packet.at(start) == '\0' || std::isspace(quint8(packet.at(start))))) ++start;
  QXmlStreamReader xml(packet.mid(start));

  enum class Property { None, Subject, Hierarchical, TagsList, Rating, Description };
  Property current = Property::None;
  int depth = 0;
  int propertyDepth = 0;
  int itemDepth = 0;
  bool sawItem = false;
  bool sawRdf = false;
  bool closed = false;
  QString text;
  QString language;
  QStringList subjects, hierarchical, tagsList;
  std::vector<std::pair<QString, QString>> descriptions; // language, text
  QString ratingText;
  bool hasRating = false;

  const auto push = [&](const QString& value, const QString& lang) {
    switch (current) {
    case Property::Subject: subjects.append(value); break;
    case Property::Hierarchical: hierarchical.append(value); break;
    case Property::TagsList: tagsList.append(value); break;
    case Property::Rating:
      if (!hasRating) {
        ratingText = value;
        hasRating = true;
      }
      break;
    case Property::Description: descriptions.emplace_back(lang, value); break;
    case Property::None: break;
    }
  };

  while (!closed && !xml.atEnd()) {
    switch (xml.readNext()) {
    case QXmlStreamReader::DTD:
    case QXmlStreamReader::EntityReference:
      return {};
    case QXmlStreamReader::StartElement: {
      if (++depth > kMaximumXmlDepth) return {};
      const QStringView ns = xml.namespaceUri();
      const QStringView name = xml.name();
      if (ns == kRdf && name == u"RDF") sawRdf = true;
      const QXmlStreamAttributes attributes = xml.attributes();
      if (current == Property::None) {
        // Simple properties written as attributes of rdf:Description.
        for (const QXmlStreamAttribute& attribute : attributes) {
          if (attribute.namespaceUri() == kXmp && attribute.name() == u"Rating" && !hasRating) {
            ratingText = attribute.value().toString();
            hasRating = true;
          } else if (attribute.namespaceUri() == kDc && attribute.name() == u"description") {
            descriptions.emplace_back(QString(), attribute.value().toString());
          }
        }
        Property kind = Property::None;
        if (ns == kDc && name == u"subject") kind = Property::Subject;
        else if (ns == kDc && name == u"description") kind = Property::Description;
        else if (ns == kLightroom && name == u"hierarchicalSubject") kind = Property::Hierarchical;
        else if (ns == kDigiKam && name == u"TagsList") kind = Property::TagsList;
        else if (ns == kXmp && name == u"Rating") kind = Property::Rating;
        if (kind != Property::None) {
          current = kind;
          propertyDepth = depth;
          sawItem = false;
          text.clear();
        }
      } else if (itemDepth == 0 && ns == kRdf && name == u"li") {
        itemDepth = depth;
        sawItem = true;
        text.clear();
        language.clear();
        for (const QXmlStreamAttribute& attribute : attributes) {
          if (attribute.name() == u"lang" &&
              (attribute.namespaceUri() == kXmlNamespace || attribute.qualifiedName() == u"xml:lang")) {
            language = attribute.value().toString();
          }
        }
      }
      break;
    }
    case QXmlStreamReader::Characters:
      if (current != Property::None && (depth == itemDepth || (itemDepth == 0 && depth == propertyDepth)))
        text += xml.text();
      break;
    case QXmlStreamReader::EndElement:
      if (itemDepth != 0 && depth == itemDepth) {
        push(text, language);
        itemDepth = 0;
        text.clear();
      } else if (current != Property::None && depth == propertyDepth) {
        if (!sawItem) push(text, {});
        current = Property::None;
        text.clear();
      }
      if (--depth == 0) closed = true;
      break;
    default:
      break;
    }
  }
  // Anything after the root element is not looked at, so trailing padding or
  // junk does not cost a packet that was whole.
  if (!closed || !sawRdf) return {};

  Fields fields;
  fields.hasXmp = true;
  fields.keywords = xmpKeywords(hierarchical, tagsList, subjects);
  fields.rating = hasRating ? ratingFrom(ratingText) : 0;
  const std::pair<QString, QString>* chosen = nullptr;
  for (const auto& entry : descriptions) {
    if (entry.first.compare(u"x-default", Qt::CaseInsensitive) == 0) {
      chosen = &entry;
      break;
    }
  }
  if (!chosen && !descriptions.empty()) chosen = &descriptions.front();
  if (chosen) fields.description = descriptionFrom(chosen->second);
  return fields;
}

Fields parseIptc(const QByteArray& records) {
  QList<QByteArray> keywords;
  QByteArray caption;
  bool hasCaption = false;
  bool utf8 = false;
  const qint64 size = records.size();
  qint64 at = 0;
  while (at + 5 <= size && quint8(records.at(at)) == 0x1C) {
    const quint8 record = quint8(records.at(at + 1));
    const quint8 dataset = quint8(records.at(at + 2));
    qint64 length = be16(records, at + 3);
    qint64 data = at + 5;
    if (length & 0x8000) { // extended: the next |length| bytes hold the length
      const qint64 lengthBytes = length & 0x7FFF;
      if (lengthBytes < 1 || lengthBytes > 4 || data + lengthBytes > size) break;
      length = 0;
      for (qint64 index = 0; index < lengthBytes; ++index) length = length << 8 | quint8(records.at(data + index));
      data += lengthBytes;
    }
    if (length > size - data) break;
    const QByteArray value = records.mid(data, length);
    if (record == 1 && dataset == 90 && value.contains(QByteArrayLiteral("\x1B%G"))) utf8 = true;
    if (record == 2 && dataset == 25 && keywords.size() < 4 * kMaximumKeywords) keywords.append(value);
    if (record == 2 && dataset == 120 && !hasCaption) {
      caption = value;
      hasCaption = true;
    }
    at = data + length;
  }
  if (!utf8) {
    utf8 = validUtf8(caption) && std::all_of(keywords.cbegin(), keywords.cend(), validUtf8);
  }
  const auto decode = [utf8](const QByteArray& bytes) {
    return utf8 ? QString::fromUtf8(bytes) : QString::fromLatin1(bytes);
  };
  Fields fields;
  KeywordList list;
  for (const QByteArray& value : std::as_const(keywords)) {
    const QString raw = decode(value).trimmed();
    if (!tooLong(raw)) list.add(TagNames::normalized(raw));
  }
  fields.keywords = list.names();
  fields.description = descriptionFrom(decode(caption));
  return fields;
}

Packets jpegPackets(const QByteArray& head) {
  BytesSource source(head);
  return jpegWalk(source);
}

Packets pngPackets(const QByteArray& head) {
  BytesSource source(head);
  return pngWalk(source);
}

Packets webpPackets(const QByteArray& head) {
  BytesSource source(head);
  return webpWalk(source);
}

Packets tiffPackets(const QByteArray& head) {
  BytesSource source(head);
  return tiffWalk(source);
}

Packets packets(const QByteArray& head) {
  BytesSource source(head);
  return sniffAndWalk(source);
}

Fields readFile(const QString& path) {
  const QString suffix = QFileInfo(path).suffix();
  if (CaptureScanner::isVideo(suffix) || CaptureScanner::isDocument(suffix)) return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  FileSource source(file);
  const Packets found = sniffAndWalk(source);
  Fields fields = found.xmp.isEmpty() ? Fields{} : parseXmp(found.xmp);
  if (!found.iptc.isEmpty()) {
    const Fields iptc = parseIptc(found.iptc);
    if (fields.description.isEmpty()) fields.description = iptc.description;
    if (!fields.hasXmp) fields.keywords = iptc.keywords;
  }
  return fields;
}

Fields read(const QString& path, const QString& sidecar) {
  Fields side;
  if (!sidecar.isEmpty()) {
    QFile file(sidecar);
    if (file.open(QIODevice::ReadOnly) && file.size() <= kMaximumPacketBytes) {
      side = parseXmp(file.read(kMaximumPacketBytes + 1));
    }
  }
  const Fields own = readFile(path);
  Fields fields;
  fields.hasXmp = side.hasXmp || own.hasXmp;
  fields.rating = side.rating != 0 ? side.rating : own.rating;
  fields.description = !side.description.isEmpty() ? side.description : own.description;
  // The file's keywords are its XMP's, or its IPTC's when it has no XMP;
  // those count only when the sidecar has no XMP either.
  if (!side.keywords.isEmpty()) fields.keywords = side.keywords;
  else if (own.hasXmp || !side.hasXmp) fields.keywords = own.keywords;
  return fields;
}

namespace {
// "IMG_1.CR2" is "IMG_1"; only the last extension goes.
QString completeBaseName(const QString& fileName) {
  const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
  return dot <= 0 ? fileName : fileName.left(dot);
}
bool isXmpName(const QString& fileName) { return fileName.endsWith(u".xmp", Qt::CaseInsensitive); }
} // namespace

QString sidecarName(const QString& fileName, bool raw, bool baseNameShared,
                    const QHash<QString, QString>& sidecarsByLowerName) {
  if (fileName.isEmpty() || isXmpName(fileName)) return {};
  const QString full = sidecarsByLowerName.value(fileName.toLower() + QStringLiteral(".xmp"));
  if (!full.isEmpty()) return full;
  // Shared means another file could own it: another raw for a raw (RAW+JPEG
  // pairs give it to the raw), any other media file for anything else.
  (void)raw;
  if (baseNameShared) return {};
  const QString base = completeBaseName(fileName);
  if (base == fileName) return {};
  return sidecarsByLowerName.value(base.toLower() + QStringLiteral(".xmp"));
}

QString findSidecar(const QString& path) {
  const QFileInfo info(path);
  const QString fileName = info.fileName();
  if (fileName.isEmpty() || isXmpName(fileName)) return {};
  const QDir dir = info.absoluteDir();
  const QStringList entries = dir.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
  const QString base = completeBaseName(fileName).toLower();
  const bool raw = CameraRaw::isRaw(info.suffix());
  QHash<QString, QString> sidecars;
  bool shared = false;
  for (const QString& entry : entries) {
    if (isXmpName(entry)) {
      sidecars.insert(entry.toLower(), entry);
    } else if (entry != fileName && completeBaseName(entry).toLower() == base) {
      // Another owner, as the scanner counts them: another raw for a raw;
      // for anything else any media file, and a file with no extension too,
      // which the scanner may take for media by its content.
      const QString suffix = QFileInfo(entry).suffix();
      shared = shared || (raw ? CameraRaw::isRaw(suffix)
                              : suffix.isEmpty() || CaptureScanner::isSupported(suffix));
    }
  }
  if (sidecars.isEmpty()) return {};
  const QString name = sidecarName(fileName, raw, shared, sidecars);
  return name.isEmpty() ? QString() : dir.absoluteFilePath(name);
}

} // namespace EmbeddedMetadata
