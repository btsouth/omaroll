#include "viewer/PictureDiagnosis.h"

#include "sources/ImageBudget.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>

#include <cstring>

namespace PictureDiagnosis {
namespace {

const QString kCutOff = QStringLiteral(
    "The file ends early, so part of the picture is missing. A download or copy "
    "may not have finished.");

quint32 bigEndian32(const uchar* data) {
  return (quint32(data[0]) << 24) | (quint32(data[1]) << 16) | (quint32(data[2]) << 8) |
         quint32(data[3]);
}

enum class Ending { Complete, CutOff, Garbled };

// Walks the segments and the entropy-coded data between them to the
// end-of-image marker. Data after it, such as the video a motion photo
// appends, is fine. Running out of bytes first is a cut-off file; something
// other than a marker where one belongs is a damaged one.
Ending jpegEnding(const uchar* data, qint64 size) {
  if (size >= 2 && data[size - 2] == 0xFF && data[size - 1] == 0xD9) return Ending::Complete;
  qint64 at = 2;
  while (at < size) {
    if (data[at] != 0xFF) return Ending::Garbled;
    while (at < size && data[at] == 0xFF) ++at;
    if (at >= size) return Ending::CutOff;
    const uchar marker = data[at++];
    if (marker == 0xD9) return Ending::Complete;
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (marker == 0x00) return Ending::Garbled;
    if (at + 2 > size) return Ending::CutOff;
    const qint64 length = (qint64(data[at]) << 8) | data[at + 1];
    if (length < 2) return Ending::Garbled;
    at += length;
    if (at > size) return Ending::CutOff;
    if (marker != 0xDA) continue;
    // Scan data: a marker is 0xFF followed by anything but a stuffed zero
    // or a restart marker.
    while (at + 1 < size) {
      if (data[at] == 0xFF && data[at + 1] != 0x00 &&
          !(data[at + 1] >= 0xD0 && data[at + 1] <= 0xD7)) {
        break;
      }
      ++at;
    }
    if (at + 1 >= size) return Ending::CutOff;
  }
  return Ending::CutOff;
}

Ending pngEnding(const uchar* data, qint64 size) {
  static const uchar iend[] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
  if (size >= 12 && memcmp(data + size - 12, iend, 12) == 0) return Ending::Complete;
  qint64 at = 8;
  while (at + 8 <= size) {
    const qint64 length = bigEndian32(data + at);
    for (int i = 4; i < 8; ++i) {
      const uchar letter = data[at + i];
      if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')))
        return Ending::Garbled;
    }
    if (memcmp(data + at + 4, "IEND", 4) == 0) return Ending::Complete;
    at += 12 + length;
  }
  return Ending::CutOff;
}

// "Plain text document" reads as "a plain text document"; "JPEG image"
// keeps its capitals.
QString article(QString noun) {
  if (noun.isEmpty()) return noun;
  if (noun.size() > 1 && noun.at(1).isLower()) noun[0] = noun.at(0).toLower();
  const QChar first = noun.at(0).toLower();
  return (QStringLiteral("aeiou").contains(first) ? QStringLiteral("an ") : QStringLiteral("a ")) +
         noun;
}

} // namespace

QString truncation(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  const qint64 size = file.size();
  if (size < 8) return {};
  const uchar* data = file.map(0, size);
  if (!data) return {};
  const bool jpeg = data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
  static const uchar signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  const bool png = memcmp(data, signature, 8) == 0;
  const Ending ending = jpeg ? jpegEnding(data, size) : png ? pngEnding(data, size) : Ending::Complete;
  file.unmap(const_cast<uchar*>(data));
  return ending == Ending::CutOff ? kCutOff : QString();
}

Finding failure(const QString& path) {
  const QFileInfo info(path);
  if (!info.exists()) {
    return {QStringLiteral("missing"), QStringLiteral("This file is no longer here"),
            QStringLiteral("It was moved, renamed or deleted after it was opened.")};
  }
  if (!info.isReadable()) {
    return {QStringLiteral("unreadable"), QStringLiteral("Omaroll can't read this file"),
            QStringLiteral("You don't have permission to open it.")};
  }
  if (info.size() == 0) {
    return {QStringLiteral("empty"), QStringLiteral("This file is empty"),
            QStringLiteral("It holds no picture. A download or copy may not have finished.")};
  }
  const QMimeDatabase mimes;
  const QMimeType content = mimes.mimeTypeForFile(path, QMimeDatabase::MatchContent);
  const QMimeType named = mimes.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
  const bool contentKnown = content.isValid() && !content.isDefault();
  const QString namedFormat = named.isValid() && !named.isDefault() ? named.comment()
                                                                    : QStringLiteral("picture");
  QImageReader reader(path);
  if (!reader.canRead()) {
    if (contentKnown && !content.name().startsWith(u"image/")) {
      return {QStringLiteral("not-picture"), QStringLiteral("This file isn't a picture"),
              QStringLiteral("Its contents look like %1, not %2.")
                  .arg(article(content.comment()), article(namedFormat))};
    }
    const QStringList readable = [] {
      QStringList names;
      for (const QByteArray& type : QImageReader::supportedMimeTypes())
        names.append(QString::fromLatin1(type));
      return names;
    }();
    const QMimeType format = contentKnown ? content : named;
    if (format.isValid() && !format.isDefault() && !readable.contains(format.name())) {
      return {QStringLiteral("unsupported"), QStringLiteral("This format isn't supported"),
              QStringLiteral("No installed image plugin reads %1. The qt6-imageformats and "
                             "kimageformats packages add more formats.")
                  .arg(article(format.comment()))};
    }
    const QString reason = reader.errorString();
    return {QStringLiteral("damaged"), QStringLiteral("This file is damaged"),
            QStringLiteral("Its header can't be read%1.")
                .arg(reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reason))};
  }
  const ImageBudget::Estimate estimate = ImageBudget::estimate(reader);
  const QString refusal = ImageBudget::decodeRefusal(estimate);
  if (!refusal.isEmpty()) {
    return {QStringLiteral("too-large"), QStringLiteral("This picture is too large to open"),
            refusal + QStringLiteral(". Another app may show a smaller copy.")};
  }
  const QImage image = ImageBudget::readBounded(reader);
  if (image.isNull()) {
    const QString reason = reader.errorString();
    switch (reader.error()) {
    case QImageReader::UnsupportedFormatError:
      return {QStringLiteral("unsupported"), QStringLiteral("This format isn't supported"),
              QStringLiteral("The image plugin for it can't read this file (%1).").arg(reason)};
    case QImageReader::DeviceError:
      return {QStringLiteral("unreadable"), QStringLiteral("Omaroll can't read this file"),
              QStringLiteral("Reading it failed (%1).").arg(reason)};
    default:
      if (!truncation(path).isEmpty()) {
        return {QStringLiteral("truncated"), QStringLiteral("This file is cut off"), kCutOff};
      }
      return {QStringLiteral("damaged"), QStringLiteral("This file is damaged"),
              QStringLiteral("Its picture data can't be decoded%1.")
                  .arg(reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reason))};
    }
  }
  return {QStringLiteral("unknown"), QStringLiteral("Could not display this picture"),
          QStringLiteral("It decodes, but could not be drawn. It may be too large for the "
                         "graphics card, or it changed while it was opening.")};
}

} // namespace PictureDiagnosis
