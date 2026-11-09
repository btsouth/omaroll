#include "edit/JpegExif.h"

#include <functional>

namespace {

constexpr int kSoi = 0xD8;
constexpr int kApp0 = 0xE0;
constexpr int kApp1 = 0xE1;
constexpr int kApp13 = 0xED;
constexpr int kComment = 0xFE;

int byteAt(const QByteArray& data, qsizetype at) { return static_cast<quint8>(data.at(at)); }

// Calls |visit|(start, marker, payload, payloadLength) for each segment
// before the first scan, stopping when it returns false or the data ends.
void forEachSegment(const QByteArray& jpeg,
                    const std::function<bool(qsizetype, int, qsizetype, qsizetype)>& visit) {
  if (jpeg.size() < 4 || byteAt(jpeg, 0) != 0xFF || byteAt(jpeg, 1) != kSoi) return;
  qsizetype at = 2;
  while (at + 4 <= jpeg.size()) {
    if (byteAt(jpeg, at) != 0xFF) return;
    const int marker = byteAt(jpeg, at + 1);
    if (marker == 0xFF) {
      ++at; // fill byte
      continue;
    }
    if (marker == kSoi || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      at += 2; // no length
      continue;
    }
    if (marker == 0xDA || marker == 0xD9) return; // scan or end
    const qsizetype length = (byteAt(jpeg, at + 2) << 8) | byteAt(jpeg, at + 3);
    if (length < 2 || at + 2 + length > jpeg.size()) return;
    if (!visit(at, marker, at + 4, length - 2)) return;
    at += 2 + length;
  }
}

// A TIFF structure inside an EXIF segment, read and written in its own byte
// order. Every access is checked against the segment.
class Tiff {
public:
  Tiff(QByteArray& data, qsizetype base, qsizetype length)
      : m_data(data), m_base(base), m_length(length) {
    if (length < 8) return;
    const char* head = data.constData() + base;
    if (head[0] == 'I' && head[1] == 'I') m_little = true;
    else if (head[0] == 'M' && head[1] == 'M') m_little = false;
    else return;
    m_valid = read16(2) == 42;
  }

  [[nodiscard]] bool valid() const { return m_valid; }
  [[nodiscard]] bool fits(qint64 offset, qint64 size) const {
    return offset >= 0 && size >= 0 && offset + size <= m_length;
  }

  [[nodiscard]] quint32 read16(qint64 offset) const {
    if (!fits(offset, 2)) return 0;
    const int a = byteAt(m_data, m_base + offset);
    const int b = byteAt(m_data, m_base + offset + 1);
    return m_little ? quint32(a | (b << 8)) : quint32((a << 8) | b);
  }
  [[nodiscard]] quint32 read32(qint64 offset) const {
    if (!fits(offset, 4)) return 0;
    const quint32 a = read16(offset);
    const quint32 b = read16(offset + 2);
    return m_little ? (a | (b << 16)) : ((a << 16) | b);
  }
  void write16(qint64 offset, quint32 value) {
    if (!fits(offset, 2)) return;
    char* at = m_data.data() + m_base + offset;
    at[m_little ? 0 : 1] = char(value & 0xFF);
    at[m_little ? 1 : 0] = char((value >> 8) & 0xFF);
  }
  void write32(qint64 offset, quint32 value) {
    if (!fits(offset, 4)) return;
    write16(m_little ? offset : offset + 2, value & 0xFFFF);
    write16(m_little ? offset + 2 : offset, value >> 16);
  }

  // Calls |visit|(entryOffset, tag, type) for each entry of the IFD at
  // |offset|, and returns the offset of its next-IFD link, or -1.
  qint64 forEachEntry(qint64 offset, const std::function<void(qint64, quint32, quint32)>& visit) {
    if (!fits(offset, 2)) return -1;
    const qint64 count = read16(offset);
    for (qint64 i = 0; i < count; ++i) {
      const qint64 entry = offset + 2 + 12 * i;
      if (!fits(entry, 12)) return -1;
      visit(entry, read16(entry), read16(entry + 2));
    }
    const qint64 next = offset + 2 + 12 * count;
    return fits(next, 4) ? next : -1;
  }

private:
  QByteArray& m_data;
  qsizetype m_base = 0;
  qsizetype m_length = 0;
  bool m_little = false;
  bool m_valid = false;
};

constexpr quint32 kShort = 3;
constexpr quint32 kLong = 4;

} // namespace

namespace JpegExif {

bool normalize(QByteArray& jpeg, const QSize& size) {
  qsizetype base = -1;
  qsizetype length = 0;
  forEachSegment(jpeg, [&](qsizetype, int marker, qsizetype payload, qsizetype payloadLength) {
    if (marker == kApp1 && payloadLength > 6 && jpeg.mid(payload, 6) == QByteArray("Exif\0\0", 6)) {
      base = payload + 6;
      length = payloadLength - 6;
      return false;
    }
    return true;
  });
  if (base < 0) return false;

  Tiff tiff(jpeg, base, length);
  if (!tiff.valid()) return true;
  qint64 exifIfd = -1;
  const qint64 next = tiff.forEachEntry(tiff.read32(4), [&](qint64 entry, quint32 tag, quint32 type) {
    if (tag == 0x0112 && type == kShort) tiff.write16(entry + 8, 1);
    if (tag == 0x8769 && type == kLong) exifIfd = tiff.read32(entry + 8);
  });
  // IFD1 is the embedded preview, still in the old orientation.
  if (next >= 0) tiff.write32(next, 0);
  if (exifIfd > 0 && size.isValid()) {
    tiff.forEachEntry(exifIfd, [&](qint64 entry, quint32 tag, quint32 type) {
      if (tag != 0xA002 && tag != 0xA003) return;
      const quint32 value = quint32(tag == 0xA002 ? size.width() : size.height());
      if (type == kLong) tiff.write32(entry + 8, value);
      else if (type == kShort && value <= 0xFFFF) tiff.write16(entry + 8, value);
    });
  }
  return true;
}

QByteArray metadataSegments(const QByteArray& jpeg) {
  QByteArray segments;
  forEachSegment(jpeg, [&](qsizetype start, int marker, qsizetype, qsizetype payloadLength) {
    if (marker == kApp1 || marker == kApp13 || marker == kComment) {
      segments.append(jpeg.mid(start, 4 + payloadLength));
    }
    return true;
  });
  return segments;
}

QByteArray withSegments(const QByteArray& jpeg, const QByteArray& segments) {
  if (segments.isEmpty() || jpeg.size() < 4 || byteAt(jpeg, 0) != 0xFF || byteAt(jpeg, 1) != kSoi) {
    return jpeg;
  }
  qsizetype at = 2;
  forEachSegment(jpeg, [&](qsizetype start, int marker, qsizetype, qsizetype payloadLength) {
    if (start == 2 && marker == kApp0) at = start + 4 + payloadLength;
    return false;
  });
  return jpeg.left(at) + segments + jpeg.mid(at);
}

} // namespace JpegExif
