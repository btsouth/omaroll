#include "sources/CameraRaw.h"

#include "sources/CaptureScanner.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QSet>
#include <QTransform>

#include <cmath>
#include <cstdlib>
#include <optional>

namespace {

// The plugin also claims "raw", but that name is shared with headerless pixel
// dumps and disk images, which would only ever fail here.
const QSet<QString> kRawSuffixes = {
    QStringLiteral("3fr"), QStringLiteral("arw"), QStringLiteral("cr2"), QStringLiteral("cr3"),
    QStringLiteral("crw"), QStringLiteral("dcr"), QStringLiteral("dng"), QStringLiteral("erf"),
    QStringLiteral("fff"), QStringLiteral("iiq"), QStringLiteral("kdc"), QStringLiteral("mef"),
    QStringLiteral("mos"), QStringLiteral("mrw"), QStringLiteral("nef"), QStringLiteral("nrw"),
    QStringLiteral("orf"), QStringLiteral("pef"), QStringLiteral("raf"), QStringLiteral("rw2"),
    QStringLiteral("rwl"), QStringLiteral("sr2"), QStringLiteral("srf"), QStringLiteral("srw"),
    QStringLiteral("x3f"),
};

// Kimageformats' raw plugin reads quality 0 as "the embedded preview" and
// 20 to 29 as a full-size AHD demosaic at 8 bits per channel. Its default is
// the same demosaic at 16 bits, twice the memory for nothing a screen shows.
constexpr int kPreviewQuality = 0;
constexpr int kFullQuality = 20;

// Bounds for a hostile or truncated file: no IFD has this many entries, and
// no text field the library shows is this long.
constexpr int kMaximumEntries = 1024;
constexpr quint32 kMaximumText = 256;
constexpr int kMaximumBoxes = 64;

enum Tag : quint16 {
  Make = 0x010F,
  Model = 0x0110,
  Orientation = 0x0112,
  ExposureTime = 0x829A,
  FNumber = 0x829D,
  ExifPointer = 0x8769,
  Iso = 0x8827,
  DateTimeOriginal = 0x9003,
  DateTimeDigitized = 0x9004,
  FocalLength = 0x920A,
  LensModel = 0xA434,
};

enum Type : quint16 {
  Byte = 1,
  Ascii = 2,
  Short = 3,
  Long = 4,
  Rational = 5,
  SignedRational = 10,
};

// Random access into one TIFF structure, which may sit inside a larger file:
// a CR3 box or the Exif segment of the JPEG at the front of a RAF. Offsets in
// the structure count from its own header.
class Tiff {
public:
  Tiff(QFile& file, qint64 base) : m_file(file), m_base(base) {}

  bool open() {
    const QByteArray header = read(0, 8);
    if (header.size() != 8) {
      return false;
    }
    // ORF writes "IIRO" and RW2 "IIU\0" in place of TIFF's 42; the
    // structure after it is ordinary TIFF.
    if (header.startsWith("II")) {
      m_bigEndian = false;
    } else if (header.startsWith("MM")) {
      m_bigEndian = true;
    } else {
      return false;
    }
    m_first = number32(header, 4);
    return true;
  }

  [[nodiscard]] quint32 firstDirectory() const { return m_first; }

  struct Entry {
    quint16 type = 0;
    quint32 count = 0;
    // The four value bytes, which hold the value itself or its offset.
    QByteArray value;
  };

  // Tag to entry for one IFD. Empty when the offset leads nowhere.
  [[nodiscard]] QHash<quint16, Entry> directory(quint32 offset) const {
    QHash<quint16, Entry> entries;
    const QByteArray countBytes = read(offset, 2);
    if (offset == 0 || countBytes.size() != 2) {
      return entries;
    }
    const int count = number16(countBytes, 0);
    if (count <= 0 || count > kMaximumEntries) {
      return entries;
    }
    const QByteArray table = read(qint64(offset) + 2, qint64(count) * 12);
    if (table.size() != count * 12) {
      return entries;
    }
    for (int index = 0; index < count; ++index) {
      const qsizetype at = qsizetype(index) * 12;
      entries.insert(number16(table, at),
                     {number16(table, at + 2), number32(table, at + 4), table.mid(at + 8, 4)});
    }
    return entries;
  }

  [[nodiscard]] QString text(const Entry& entry) const {
    if (entry.type != Ascii || entry.count == 0 || entry.count > kMaximumText) {
      return {};
    }
    QByteArray bytes = entry.count <= 4 ? entry.value.left(int(entry.count))
                                        : read(number32(entry.value, 0), entry.count);
    const qsizetype end = bytes.indexOf('\0');
    if (end >= 0) {
      bytes.truncate(end);
    }
    return QString::fromUtf8(bytes).simplified();
  }

  [[nodiscard]] std::optional<quint32> integer(const Entry& entry) const {
    if (entry.count == 0) {
      return std::nullopt;
    }
    switch (entry.type) {
    case Byte:
      return quint8(entry.value.at(0));
    case Short:
      return number16(entry.value, 0);
    case Long:
      return number32(entry.value, 0);
    default:
      return std::nullopt;
    }
  }

  // The first rational as numerator and denominator.
  [[nodiscard]] std::optional<std::pair<double, double>> rational(const Entry& entry) const {
    if ((entry.type != Rational && entry.type != SignedRational) || entry.count == 0) {
      return std::nullopt;
    }
    const QByteArray bytes = read(number32(entry.value, 0), 8);
    if (bytes.size() != 8) {
      return std::nullopt;
    }
    const quint32 numerator = number32(bytes, 0);
    const quint32 denominator = number32(bytes, 4);
    if (denominator == 0) {
      return std::nullopt;
    }
    if (entry.type == SignedRational) {
      return std::pair<double, double>(qint32(numerator), qint32(denominator));
    }
    return std::pair<double, double>(numerator, denominator);
  }

private:
  [[nodiscard]] QByteArray read(qint64 offset, qint64 length) const {
    const qint64 at = m_base + offset;
    if (offset < 0 || length <= 0 || at + length > m_file.size() || !m_file.seek(at)) {
      return {};
    }
    return m_file.read(length);
  }

  [[nodiscard]] quint16 number16(const QByteArray& bytes, qsizetype at) const {
    const auto byte = [&bytes, at](int index) { return quint16(quint8(bytes.at(at + index))); };
    return m_bigEndian ? quint16(byte(0) << 8 | byte(1)) : quint16(byte(1) << 8 | byte(0));
  }

  [[nodiscard]] quint32 number32(const QByteArray& bytes, qsizetype at) const {
    const auto byte = [&bytes, at](int index) { return quint32(quint8(bytes.at(at + index))); };
    return m_bigEndian ? byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3)
                       : byte(3) << 24 | byte(2) << 16 | byte(1) << 8 | byte(0);
  }

  QFile& m_file;
  qint64 m_base = 0;
  bool m_bigEndian = false;
  quint32 m_first = 0;
};

QString exposureLabel(double numerator, double denominator) {
  if (numerator <= 0 || denominator <= 0) {
    return {};
  }
  // Fractions the way the camera's dial shows them, "1/125"; long exposures
  // in seconds, "2.5".
  const double seconds = numerator / denominator;
  const double reciprocal = 1.0 / seconds;
  if (seconds < 1.0 && std::abs(reciprocal - std::round(reciprocal)) < 0.05) {
    return QStringLiteral("1/%1").arg(std::lround(reciprocal));
  }
  return QString::number(std::round(seconds * 10.0) / 10.0);
}

// Fills whatever |directory| holds that is still missing, so IFD0 and the
// Exif IFD can be read in either order and a DNG's IFD0 copies count too.
void collect(const Tiff& tiff, const QHash<quint16, Tiff::Entry>& directory,
             CameraRaw::Metadata& metadata) {
  const auto found = [&directory](quint16 tag) { return directory.constFind(tag); };
  const auto setText = [&](quint16 tag, QString& field) {
    if (field.isEmpty() && found(tag) != directory.cend()) {
      field = tiff.text(*found(tag));
    }
  };
  setText(Make, metadata.make);
  setText(Model, metadata.model);
  setText(LensModel, metadata.lens);

  if (!metadata.taken.isValid()) {
    for (const quint16 tag : {DateTimeOriginal, DateTimeDigitized}) {
      if (found(tag) != directory.cend()) {
        const QDateTime parsed = QDateTime::fromString(tiff.text(*found(tag)).left(19),
                                                       QStringLiteral("yyyy:MM:dd HH:mm:ss"));
        if (parsed.isValid()) {
          metadata.taken = parsed;
          break;
        }
      }
    }
  }
  if (metadata.orientation == 1 && found(Orientation) != directory.cend()) {
    const auto value = tiff.integer(*found(Orientation));
    if (value && *value >= 1 && *value <= 8) {
      metadata.orientation = int(*value);
    }
  }
  if (metadata.iso == 0 && found(Iso) != directory.cend()) {
    metadata.iso = int(tiff.integer(*found(Iso)).value_or(0));
  }
  if (metadata.exposureTime.isEmpty() && found(ExposureTime) != directory.cend()) {
    if (const auto value = tiff.rational(*found(ExposureTime))) {
      metadata.exposureTime = exposureLabel(value->first, value->second);
    }
  }
  const auto setNumber = [&](quint16 tag, double& field) {
    if (field <= 0 && found(tag) != directory.cend()) {
      if (const auto value = tiff.rational(*found(tag))) {
        field = std::max(0.0, value->first / value->second);
      }
    }
  };
  setNumber(FNumber, metadata.fNumber);
  setNumber(FocalLength, metadata.focalLength);
}

// IFD0 and the Exif IFD it points to. With |exifOnly| the first IFD is itself
// the Exif IFD, as in a CR3's CMT2 box.
void readTiff(QFile& file, qint64 base, CameraRaw::Metadata& metadata, bool exifOnly = false) {
  Tiff tiff(file, base);
  if (!tiff.open()) {
    return;
  }
  const auto first = tiff.directory(tiff.firstDirectory());
  collect(tiff, first, metadata);
  if (exifOnly) {
    return;
  }
  const auto pointer = first.constFind(ExifPointer);
  if (pointer != first.cend()) {
    if (const auto offset = tiff.integer(*pointer)) {
      collect(tiff, tiff.directory(*offset), metadata);
    }
  }
}

quint32 bigEndian32(const QByteArray& bytes, qsizetype at) {
  return quint32(quint8(bytes.at(at))) << 24 | quint32(quint8(bytes.at(at + 1))) << 16 |
         quint32(quint8(bytes.at(at + 2))) << 8 | quint32(quint8(bytes.at(at + 3)));
}

// ISO base media boxes between |begin| and |end|: CR3 keeps its TIFF
// structures in CMT1 (IFD0) and CMT2 (Exif) boxes inside Canon's uuid box in
// moov.
void readBoxes(QFile& file, qint64 begin, qint64 end, CameraRaw::Metadata& metadata,
               int depth) {
  static const QByteArray canon = QByteArray::fromHex("85c0b687820f11e08111f4ce462b6a48");
  qint64 at = begin;
  for (int boxes = 0; boxes < kMaximumBoxes && at + 8 <= end; ++boxes) {
    if (!file.seek(at)) {
      return;
    }
    const QByteArray header = file.read(8);
    if (header.size() != 8) {
      return;
    }
    qint64 size = bigEndian32(header, 0);
    const QByteArray type = header.mid(4, 4);
    qint64 payload = at + 8;
    if (size == 1) {
      const QByteArray large = file.read(8);
      if (large.size() != 8) {
        return;
      }
      size = qint64(bigEndian32(large, 0)) << 32 | bigEndian32(large, 4);
      payload += 8;
    } else if (size == 0) {
      size = end - at;
    }
    if (size < payload - at || at + size > end) {
      return;
    }
    if (depth == 0 && type == "moov") {
      readBoxes(file, payload, at + size, metadata, 1);
    } else if (depth == 1 && type == "uuid" && file.read(16) == canon) {
      readBoxes(file, payload + 16, at + size, metadata, 2);
    } else if (depth == 2 && (type == "CMT1" || type == "CMT2")) {
      readTiff(file, payload, metadata, type == "CMT2");
    }
    at += size;
  }
}

// A RAF opens with a full JPEG whose Exif segment carries the camera's tags.
void readFujifilm(QFile& file, CameraRaw::Metadata& metadata) {
  if (!file.seek(84)) {
    return;
  }
  const QByteArray pointer = file.read(4);
  if (pointer.size() != 4) {
    return;
  }
  qint64 at = bigEndian32(pointer, 0);
  if (!file.seek(at) || file.read(2) != QByteArray::fromHex("ffd8")) {
    return;
  }
  at += 2;
  for (int segments = 0; segments < 16; ++segments) {
    if (!file.seek(at)) {
      return;
    }
    const QByteArray header = file.read(4);
    if (header.size() != 4 || quint8(header.at(0)) != 0xFF) {
      return;
    }
    const int length = quint8(header.at(2)) << 8 | quint8(header.at(3));
    if (quint8(header.at(1)) == 0xE1 && file.read(6) == QByteArray("Exif\0\0", 6)) {
      readTiff(file, at + 10, metadata);
      return;
    }
    if (length < 2) {
      return;
    }
    at += 2 + length;
  }
}

} // namespace

namespace CameraRaw {

bool isRaw(const QString& suffix) {
  const QString name = suffix.toLower();
  // Asking Qt which formats it reads loads every image plugin, so only a raw
  // name ever asks, and only once.
  if (!kRawSuffixes.contains(name)) {
    return false;
  }
  static const QSet<QString> readable = [] {
    QSet<QString> formats;
    for (const QByteArray& format : QImageReader::supportedImageFormats()) {
      const QString name = QString::fromLatin1(format).toLower();
      if (kRawSuffixes.contains(name)) {
        formats.insert(name);
      }
    }
    return formats;
  }();
  return readable.contains(name);
}

bool isRawFile(const QString& path) { return isRaw(CaptureScanner::mediaSuffix(path)); }

Metadata readMetadata(const QString& path) {
  Metadata metadata;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return metadata;
  }
  const QByteArray head = file.read(16);
  if (head.startsWith("II") || head.startsWith("MM")) {
    readTiff(file, 0, metadata);
  } else if (head.mid(4, 4) == "ftyp") {
    readBoxes(file, 0, file.size(), metadata, 0);
  } else if (head.startsWith("FUJIFILMCCD-RAW")) {
    readFujifilm(file, metadata);
  }
  return metadata;
}

QImage upright(const QImage& image, int orientation) {
  if (image.isNull() || orientation <= 1 || orientation > 8) {
    return image;
  }
  QImage result = image;
  // 2, 4, 5 and 7 are mirrored: mirror first, then turn.
  if (orientation == 2 || orientation == 5 || orientation == 7) {
    result = result.transformed(QTransform().scale(-1, 1), Qt::FastTransformation);
  } else if (orientation == 4) {
    result = result.transformed(QTransform().scale(1, -1), Qt::FastTransformation);
  }
  const int degrees = orientation == 3 ? 180
                      : orientation == 6 || orientation == 7 ? 90
                      : orientation == 5 || orientation == 8 ? 270
                                                             : 0;
  if (degrees != 0) {
    result = result.transformed(QTransform().rotate(degrees), Qt::FastTransformation);
  }
  return result;
}

QImage readPreview(const QString& path, const QSize& cover) {
  QImageReader reader(path);
  reader.setQuality(kPreviewQuality);
  const QSize full = reader.size();
  QImage image = reader.read();
  if (image.isNull()) {
    return {};
  }
  // Without an embedded preview the plugin falls back to a half-size decode,
  // which LibRaw has already turned upright; an embedded preview is stored
  // the way the sensor saw it.
  const bool decoded = full.isValid() && std::abs(image.width() - (full.width() + 1) / 2) <= 1 &&
                       std::abs(image.height() - (full.height() + 1) / 2) <= 1;
  const int orientation = decoded ? 1 : readMetadata(path).orientation;
  if (cover.isValid() && !cover.isEmpty()) {
    // Still as stored, so a quarter turn to come is a transposed target.
    const QSize target = orientation >= 5 ? cover.transposed() : cover;
    if (image.width() > target.width() && image.height() > target.height()) {
      image = image.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
  }
  return upright(image, orientation);
}

QImage readFull(const QString& path) {
  QImageReader reader(path);
  reader.setQuality(kFullQuality);
  return reader.read();
}

QSize fullSize(const QString& path) {
  if (!isRawFile(path)) {
    return {};
  }
  QImageReader reader(path);
  const QSize size = reader.size();
  return size.isValid() && !size.isEmpty() ? size : QSize();
}

namespace {

QUrl providerUrl(const QString& variant, const QString& path) {
  if (path.isEmpty()) {
    return {};
  }
  // The same shape as the thumbnail ids: a head without a slash, then the
  // absolute path encoded whole so '#', '?' and '%' survive URL parsing.
  const qint64 stamp = QFileInfo(path).lastModified().toMSecsSinceEpoch();
  return QUrl(QStringLiteral("image://raw/%1~%2").arg(variant).arg(stamp) +
              QString::fromLatin1(QUrl::toPercentEncoding(path)));
}

} // namespace

QUrl previewUrl(const QString& path) { return providerUrl(QStringLiteral("preview"), path); }

QUrl fullUrl(const QString& path) { return providerUrl(QStringLiteral("full"), path); }

} // namespace CameraRaw
