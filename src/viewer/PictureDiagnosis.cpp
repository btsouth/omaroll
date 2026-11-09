#include "viewer/PictureDiagnosis.h"

#include "sources/ImageBudget.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
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

// The file read in chunks rather than mapped: a file cut short while it is
// checked, which the viewer's file watcher reruns this for, ends the walk
// instead of faulting on a page that is gone.
class Bytes {
public:
  explicit Bytes(QFile& file) : m_file(file), m_size(file.size()) {}
  [[nodiscard]] qint64 size() const { return m_size; }

  // The byte at |pos|, or -1 past the end.
  int at(qint64 pos) {
    if (pos < 0 || pos >= m_size) return -1;
    if (pos < m_base || pos >= m_base + m_chunk.size()) {
      m_base = pos;
      m_chunk = m_file.seek(pos) ? m_file.read(kChunk) : QByteArray();
      // A short read is a file that shrank since it was opened.
      if (m_chunk.size() < kChunk) m_size = std::min(m_size, pos + m_chunk.size());
      if (m_chunk.isEmpty()) return -1;
    }
    return uchar(m_chunk.at(pos - m_base));
  }

  bool read(qint64 pos, uchar* out, int count) {
    for (int i = 0; i < count; ++i) {
      const int value = at(pos + i);
      if (value < 0) return false;
      out[i] = uchar(value);
    }
    return true;
  }

private:
  static constexpr qint64 kChunk = 64 * 1024;
  QFile& m_file;
  qint64 m_size;
  qint64 m_base = 0;
  QByteArray m_chunk;
};

enum class Ending { Complete, CutOff, Garbled };

// Walks the segments and the entropy-coded data between them to the
// end-of-image marker. Data after it, such as the video a motion photo
// appends, is fine. Running out of bytes first is a cut-off file; something
// other than a marker where one belongs is a damaged one.
Ending jpegEnding(Bytes& bytes) {
  const qint64 size = bytes.size();
  if (bytes.at(size - 2) == 0xFF && bytes.at(size - 1) == 0xD9) return Ending::Complete;
  qint64 at = 2;
  for (;;) {
    int value = bytes.at(at);
    if (value < 0) return Ending::CutOff;
    if (value != 0xFF) return Ending::Garbled;
    while ((value = bytes.at(at)) == 0xFF) ++at;
    if (value < 0) return Ending::CutOff;
    const int marker = value;
    ++at;
    if (marker == 0xD9) return Ending::Complete;
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (marker == 0x00) return Ending::Garbled;
    const int high = bytes.at(at);
    const int low = bytes.at(at + 1);
    if (high < 0 || low < 0) return Ending::CutOff;
    const qint64 length = (qint64(high) << 8) | low;
    if (length < 2) return Ending::Garbled;
    at += length;
    if (at > bytes.size()) return Ending::CutOff;
    if (marker != 0xDA) continue;
    // Scan data: a marker is 0xFF followed by anything but a stuffed zero
    // or a restart marker.
    for (;;) {
      const int first = bytes.at(at);
      const int second = bytes.at(at + 1);
      if (first < 0 || second < 0) return Ending::CutOff;
      if (first == 0xFF && second != 0x00 && !(second >= 0xD0 && second <= 0xD7)) break;
      ++at;
    }
  }
}

Ending pngEnding(Bytes& bytes) {
  static const uchar iend[] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
  uchar tail[12];
  if (bytes.read(bytes.size() - 12, tail, 12) && memcmp(tail, iend, 12) == 0) {
    return Ending::Complete;
  }
  qint64 at = 8;
  uchar head[8];
  while (bytes.read(at, head, 8)) {
    const qint64 length = bigEndian32(head);
    for (int i = 4; i < 8; ++i) {
      const uchar letter = head[i];
      if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')))
        return Ending::Garbled;
    }
    if (memcmp(head + 4, "IEND", 4) == 0) return Ending::Complete;
    at += 12 + length;
  }
  return Ending::CutOff;
}

// "Plain text document" reads as "a plain text document"; "JPEG image"
// keeps its capitals. A leading abbreviation that is spelled out goes by how
// its first letter sounds: "an MPEG-4 video", "an SVG image", "a USB drive";
// one read as a word does not: "a HEIF image".
QString article(QString noun) {
  if (noun.isEmpty()) return noun;
  if (noun.size() > 1 && noun.at(1).isLower()) noun[0] = noun.at(0).toLower();
  const QChar first = noun.at(0).toLower();
  bool vowel = QStringLiteral("aeiou").contains(first);
  const QString word = noun.section(QLatin1Char(' '), 0, 0).section(QLatin1Char('-'), 0, 0);
  if (word.size() > 1 && word.at(0).isUpper() && word.at(1).isUpper()) {
    const bool spelled = !QStringLiteral("AEIOU").contains(word.at(1)) && word != u"FLAC";
    if (first == u'u') vowel = false;
    else if (spelled) vowel = QStringLiteral("aefhilmnorsx").contains(first);
  }
  return (vowel ? QStringLiteral("an ") : QStringLiteral("a ")) + noun;
}

} // namespace

QString truncation(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  Bytes bytes(file);
  uchar head[8];
  if (bytes.size() < 8 || !bytes.read(0, head, 8)) return {};
  const bool jpeg = head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
  static const uchar signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  const bool png = memcmp(head, signature, 8) == 0;
  const Ending ending = jpeg ? jpegEnding(bytes) : png ? pngEnding(bytes) : Ending::Complete;
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

Finding videoFailure(const QString& path, const QString& playerError, const QString& ffmpeg) {
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
            QStringLiteral("It holds no video. A download or copy may not have finished.")};
  }
  const QMimeDatabase mimes;
  const QMimeType content = mimes.mimeTypeForFile(path, QMimeDatabase::MatchContent);
  const QMimeType named = mimes.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
  const QString namedFormat = named.isValid() && !named.isDefault() ? named.comment()
                                                                    : QStringLiteral("video");
  // Some video containers have application/ types: ASF (WMV), MXF and
  // RealMedia. A content type the name's own type builds on counts as well.
  const bool media = content.name().startsWith(u"video/") || content.name().startsWith(u"audio/") ||
                     content.inherits(QStringLiteral("video/x-matroska")) ||
                     content.inherits(QStringLiteral("application/ogg")) ||
                     content.inherits(QStringLiteral("application/vnd.ms-asf")) ||
                     content.inherits(QStringLiteral("application/mxf")) ||
                     content.inherits(QStringLiteral("application/vnd.rn-realmedia")) ||
                     (named.isValid() && named.inherits(content.name()));
  if (content.isValid() && !content.isDefault() && !media) {
    return {QStringLiteral("not-video"), QStringLiteral("This file isn't a video"),
            QStringLiteral("Its contents look like %1, not %2.")
                .arg(article(content.comment()), article(namedFormat))};
  }
  const QString reported = playerError.trimmed();
  const auto damaged = [&](const QString& reason) -> Finding {
    return {QStringLiteral("damaged"), QStringLiteral("This video is damaged"),
            QStringLiteral("Its data can't be read%1. A download or copy may not have finished.")
                .arg(reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reason))};
  };
  QString program = ffmpeg.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("ffmpeg")) : ffmpeg;
  if (!program.isEmpty() && !QFileInfo(program).isExecutable()) program.clear();
  if (program.isEmpty()) {
    // Without ffmpeg only the player's own words are left.
    if (reported.contains(u"decoder", Qt::CaseInsensitive) || reported.contains(u"codec", Qt::CaseInsensitive)) {
      return {QStringLiteral("unsupported"), QStringLiteral("This video's format isn't supported"),
              QStringLiteral("No installed decoder plays it (%1).").arg(reported)};
    }
    return damaged(reported);
  }
  // The first video frame, decoded and thrown away.
  QProcess probe;
  probe.setProcessChannelMode(QProcess::SeparateChannels);
  probe.setStandardOutputFile(QProcess::nullDevice());
  probe.start(program, {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                        QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path,
                        QStringLiteral("-map"), QStringLiteral("0:v:0?"), QStringLiteral("-frames:v"),
                        QStringLiteral("1"), QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")});
  if (!probe.waitForStarted(5000)) return damaged(reported);
  if (!probe.waitForFinished(15000)) {
    probe.kill();
    probe.waitForFinished(1000);
    return {QStringLiteral("unknown"), QStringLiteral("Could not play this video"),
            QStringLiteral("Reading it took too long%1.")
                .arg(reported.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reported))};
  }
  const QString errors = QString::fromUtf8(probe.readAllStandardError().left(16 * 1024));
  // The last line that says something, with ffmpeg's "[demuxer @ 0x..] " and
  // the file name taken off.
  static const QRegularExpression prefix(QStringLiteral("^\\[[^\\]]*\\]\\s*"));
  QString reason;
  for (QString line : errors.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    line.remove(prefix);
    line.replace(path + QStringLiteral(": "), QString());
    if (!line.trimmed().isEmpty()) reason = line.trimmed();
  }
  static const QRegularExpression missingDecoder(
      QStringLiteral("(?:Decoder \\(codec ([\\w-]+)\\) not found|no decoder found for:? ?([\\w-]+)?|"
                     "Decoding requested, but no decoder found for: ([\\w-]+))"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch decoder = missingDecoder.match(errors);
  if (decoder.hasMatch()) {
    QString codec = decoder.captured(1);
    if (codec.isEmpty()) codec = decoder.captured(2);
    if (codec.isEmpty()) codec = decoder.captured(3);
    return {QStringLiteral("unsupported"), QStringLiteral("This video's format isn't supported"),
            codec.isEmpty() || codec == u"none"
                ? QStringLiteral("No installed decoder plays it. Open in mpv, or another app, may still play it.")
                : QStringLiteral("No installed decoder plays its %1 video. An FFmpeg build with %1 "
                                 "support, or another app, can play it.").arg(codec)};
  }
  // Opened, but with no stream to decode, so the probe's last words are
  // about its own output.
  if (errors.contains(u"does not contain any stream")) {
    return {QStringLiteral("damaged"), QStringLiteral("This video is damaged"),
            QStringLiteral("No video can be found in it. A download or copy may not have finished.")};
  }
  // A cut-off or damaged file is named as such even when ffmpeg recovered a
  // frame and exited cleanly; only a clean read with nothing wrong is left
  // to the player's own words.
  if (errors.contains(u"moov atom not found") || errors.contains(u"End of file") ||
      errors.contains(u"partial file", Qt::CaseInsensitive) || errors.contains(u"Truncat", Qt::CaseInsensitive)) {
    return {QStringLiteral("truncated"), QStringLiteral("This video is cut off"),
            QStringLiteral("The file ends early, so it can't be played. A download or copy may "
                           "not have finished.")};
  }
  static const QRegularExpression damage(
      QStringLiteral("Invalid data found|error while decoding|corrupt|Invalid NAL|concealing|"
                     "missing picture|non-existing PPS|decode_slice_header error"),
      QRegularExpression::CaseInsensitiveOption);
  const bool clean = probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
  if (clean && !damage.match(errors).hasMatch()) {
    return {QStringLiteral("unknown"), QStringLiteral("Could not play this video"),
            QStringLiteral("FFmpeg reads it, but playback failed%1. Open in mpv may still play it.")
                .arg(reported.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reported))};
  }
  return damaged(reason.isEmpty() ? reported : reason);
}

} // namespace PictureDiagnosis
