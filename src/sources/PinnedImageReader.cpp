#include "sources/PinnedImageReader.h"

#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"

#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

namespace {

// Asking Qt which formats it reads loads every plugin's metadata, once.
const QSet<QByteArray>& readable() {
  static const QSet<QByteArray> formats = [] {
    QSet<QByteArray> names;
    for (const QByteArray& format : QImageReader::supportedImageFormats()) names.insert(format.toLower());
    return names;
  }();
  return formats;
}

QByteArray contentFormat(const QMimeType& type) {
  return type.isValid() && !type.isDefault() ? ImageFormats::forMimeType(type.name()) : QByteArray();
}

} // namespace

namespace ImageFormats {

QByteArray forSuffix(const QString& suffix) {
  const QString name = suffix.toLower();
  if (name.isEmpty() || !CaptureScanner::isImage(name)) return {};
  static QMutex mutex;
  static QHash<QString, QByteArray> known;
  const QMutexLocker lock(&mutex);
  const auto found = known.constFind(name);
  if (found != known.cend()) return *found;
  // Listed names no plugin registers itself. "vst" would otherwise read as
  // Visio through its MIME type.
  static const QHash<QString, QByteArray> aliases = {
      {QStringLiteral("jpe"), "jpeg"},  {QStringLiteral("dib"), "bmp"},
      {QStringLiteral("jpg2"), "jp2"},  {QStringLiteral("j2c"), "j2k"},
      {QStringLiteral("jpc"), "j2k"},   {QStringLiteral("icb"), "tga"},
      {QStringLiteral("tpic"), "tga"},  {QStringLiteral("vda"), "tga"},
      {QStringLiteral("vst"), "tga"},
  };
  QByteArray format = aliases.value(name, name.toLatin1());
  if (!readable().contains(format)) {
    // Whatever reads the MIME type the name has.
    const QMimeType type = QMimeDatabase().mimeTypeForFile(QStringLiteral("x.") + name,
                                                           QMimeDatabase::MatchExtension);
    for (const QByteArray& candidate : QImageReader::imageFormatsForMimeType(type.name().toLatin1())) {
      if (readable().contains(candidate.toLower())) {
        format = candidate.toLower();
        break;
      }
    }
  }
  // Unreadable stays named, so the reader says the format is unsupported.
  known.insert(name, format);
  return format;
}

QByteArray forMimeType(const QString& mimeType) {
  if (mimeType.isEmpty()) return {};
  static QMutex mutex;
  static QHash<QString, QByteArray> known;
  {
    const QMutexLocker lock(&mutex);
    const auto found = known.constFind(mimeType);
    if (found != known.cend()) return *found;
  }
  QByteArray format;
  for (const QByteArray& candidate : QImageReader::imageFormatsForMimeType(mimeType.toLatin1())) {
    const QString name = QString::fromLatin1(candidate).toLower();
    if (CaptureScanner::isImage(name)) {
      format = forSuffix(name);
      break;
    }
  }
  const QMutexLocker lock(&mutex);
  known.insert(mimeType, format);
  return format;
}

const QList<QByteArray>& listed() {
  static const QList<QByteArray> formats = [] {
    QList<QByteArray> names;
    for (const QString& suffix : CaptureScanner::imageSuffixes()) {
      const QByteArray format = forSuffix(suffix);
      if (readable().contains(format) && !names.contains(format)) names.append(format);
    }
    return names;
  }();
  return formats;
}

bool looksAnimated(const QString& path, const QString& suffix) {
  const QString name = suffix.toLower();
  if (name != u"gif" && name != u"webp") return false;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  const QByteArray head = file.read(12);
  return name == u"gif" ? head.startsWith("GIF87a") || head.startsWith("GIF89a")
                        : head.startsWith("RIFF") && head.mid(8, 4) == "WEBP";
}

} // namespace ImageFormats

PinnedImageReader::PinnedImageReader(const QString& path) : m_scope(path) {
  setAutoDetectImageFormat(false);
  if (DecodeJournal::isBlocked(path)) {
    m_blocked = true;
    return;
  }
  setFileName(path);
  const QString suffix = CaptureScanner::mediaSuffix(path);
  // A camera raw's decoder checks its own header; asking it first would cost
  // LibRaw a second parse.
  pin(ImageFormats::forSuffix(suffix), CameraRaw::isRaw(suffix), [&path] {
    return contentFormat(QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchContent));
  });
}

PinnedImageReader::PinnedImageReader(QIODevice* device, const QByteArray& format)
    : m_scope(QString()) {
  setAutoDetectImageFormat(false);
  setDevice(device);
  pin(format.toLower(), false, [device] {
    if (!device || !device->isOpen()) return QByteArray();
    return contentFormat(QMimeDatabase().mimeTypeForData(device->peek(16 * 1024)));
  });
}

void PinnedImageReader::pin(const QByteArray& named, bool trustName,
                            const std::function<QByteArray()>& sniff) {
  // Opened here, as the reader would: with guessing off it otherwise hands a
  // file it could not open to the decoder anyway. Without a device every
  // read fails at once.
  QIODevice* source = device();
  if (!source || (!source->isOpen() && !source->open(QIODevice::ReadOnly))) {
    setDevice(nullptr);
    return;
  }
  // Where the picture starts, before the name's decoder looks.
  const qint64 start = source->isSequential() ? 0 : source->pos();
  if (!named.isEmpty()) {
    setFormat(named);
    if (trustName || QImageReader::canRead()) {
      m_pinned = true;
      return;
    }
  }
  if (!source->isSequential()) source->seek(start);
  const QByteArray content = sniff();
  // Nothing else listed reads it.
  if (content.isEmpty() || content == named) {
    refuse();
    return;
  }
  // Drops the decoder that refused it, keeping the open device, back at the
  // picture's start: the refusing decoder may have read past it.
  if (fileName().isEmpty()) {
    setDevice(source);
    if (!source->isSequential()) source->seek(start);
  } else {
    const QString file = fileName();
    setFileName(file);
    source = device();
    if (!source || !source->open(QIODevice::ReadOnly)) {
      refuse();
      return;
    }
  }
  setFormat(content);
  m_renamed = true;
  m_pinned = QImageReader::canRead();
  if (!m_pinned) refuse();
}

void PinnedImageReader::refuse() {
  // Qt's read() does not ask canRead() again, so a decoder left attached
  // would still parse bytes it refused. Only the format's name stays, for
  // saying what the file was taken for.
  if (error() != QImageReader::UnknownError) m_refusal = errorString();
  const QByteArray name = format();
  setDevice(nullptr);
  setFormat(name);
}
