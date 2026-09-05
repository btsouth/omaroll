#include "library/MediaMetadataIndex.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace {

constexpr quint32 kCacheMagic = 0x4f4d4431; // OMD1
constexpr quint16 kCacheVersion = 3;
constexpr qint64 kMaximumCacheBytes = 16LL * 1024 * 1024;
constexpr quint32 kMaximumCacheEntries = 100'000;
constexpr int kImageBatchSize = 32;
constexpr int kImageBatchTimeoutMs = 30'000;
constexpr int kVideoProbeTimeoutMs = 5000;
constexpr auto kImageRecordPrefix = "\x1e"
                                    "OMAROLL_DATE:";

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured;
}

QString cleanValue(QString value) {
  value = value.trimmed();
  return value.compare(QStringLiteral("undefined"), Qt::CaseInsensitive) == 0 ? QString() : value;
}

QDateTime exifDate(const QString& value) {
  const QString clean = cleanValue(value);
  for (const QString& candidate : {clean, clean.left(23), clean.left(19)}) {
    for (const QString& format :
         {QStringLiteral("yyyy:MM:dd HH:mm:ss.zzz"), QStringLiteral("yyyy:MM:dd HH:mm:ss"),
          QStringLiteral("yyyy-MM-dd HH:mm:ss")}) {
      const QDateTime parsed = QDateTime::fromString(candidate, format);
      if (parsed.isValid()) {
        return parsed;
      }
    }
  }
  return {};
}

QDateTime isoDate(const QString& value) {
  const QString clean = cleanValue(value);
  QDateTime parsed = QDateTime::fromString(clean, Qt::ISODateWithMs);
  if (!parsed.isValid()) {
    parsed = QDateTime::fromString(clean, Qt::ISODate);
  }
  if (parsed.isValid() && (parsed.timeSpec() == Qt::UTC || parsed.timeSpec() == Qt::OffsetFromUTC ||
                           parsed.timeSpec() == Qt::TimeZone)) {
    return parsed.toLocalTime();
  }
  return parsed;
}

QDateTime dateFromTags(const QJsonObject& tags) {
  for (const QString& key :
       {QStringLiteral("com.apple.quicktime.creationdate"), QStringLiteral("creation_time")}) {
    const QDateTime parsed = isoDate(tags.value(key).toString());
    if (parsed.isValid()) {
      return parsed;
    }
  }
  return {};
}

QString tagValue(const QJsonObject& tags, const QStringList& keys) {
  for (const QString& key : keys) {
    const QString value = cleanValue(tags.value(key).toString());
    if (!value.isEmpty()) {
      return value;
    }
  }
  return {};
}

QString identityKey(qint64 modified, qint64 bytes, quint64 device, quint64 inode) {
  return QStringLiteral("%1:%2:%3:%4").arg(modified).arg(bytes).arg(device).arg(inode);
}

} // namespace

MediaMetadataIndex::MediaMetadataIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model),
      m_imageProgram(QStandardPaths::findExecutable(QStringLiteral("magick"))),
      m_videoProgram(QStandardPaths::findExecutable(QStringLiteral("ffprobe"))) {
  loadCache();

  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
  connect(&m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
    finishCurrent(status == QProcess::NormalExit && exitCode == 0);
  });
  connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      finishCurrent(false);
    }
  });

  m_syncTimer.setSingleShot(true);
  m_syncTimer.setInterval(100);
  connect(&m_syncTimer, &QTimer::timeout, this, &MediaMetadataIndex::sync);

  m_saveTimer.setSingleShot(true);
  // Batch many quick metadata reads into one small atomic cache write.
  m_saveTimer.setInterval(5000);
  connect(&m_saveTimer, &QTimer::timeout, this, &MediaMetadataIndex::saveCache);

  if (m_model) {
    connect(m_model, &CaptureModel::countChanged, &m_syncTimer, qOverload<>(&QTimer::start));
    if (m_model->rowCount() > 0) {
      m_syncTimer.start();
    }
  }
}

MediaMetadataIndex::~MediaMetadataIndex() {
  disconnect(&m_process, nullptr, this, nullptr);
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
  if (m_cacheDirty) {
    saveCache();
  }
}

QString MediaMetadataIndex::cachePath() {
  return cacheHome() + QStringLiteral("/omaroll/media-metadata.index");
}

QDateTime MediaMetadataIndex::parseImageDate(const QByteArray& output) {
  const QStringList fields = QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  for (int index = 0; index < 2; ++index) {
    const QDateTime parsed = exifDate(fields.value(index));
    if (parsed.isValid()) {
      return parsed;
    }
  }
  return {};
}

QString MediaMetadataIndex::cameraName(const QString& make, const QString& model) {
  const QString cleanMake = cleanValue(make).simplified();
  const QString cleanModel = cleanValue(model).simplified();
  if (cleanModel.isEmpty()) {
    return cleanMake;
  }
  if (cleanMake.isEmpty() || cleanModel.startsWith(cleanMake, Qt::CaseInsensitive)) {
    return cleanModel;
  }
  return cleanMake + QLatin1Char(' ') + cleanModel;
}

MediaMetadataIndex::Details MediaMetadataIndex::parseImageDetails(const QByteArray& output) {
  const QStringList fields = QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  Details details;
  details.captured = parseImageDate(output);
  details.camera = cameraName(fields.value(2), fields.value(3));
  details.lens = cleanValue(fields.value(4)).simplified();
  return details;
}

QHash<int, MediaMetadataIndex::Details> MediaMetadataIndex::parseImageBatch(
    const QByteArray& output, QSet<int>* completed) {
  QHash<int, Details> results;
  const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  const QString prefix = QString::fromLatin1(kImageRecordPrefix);
  for (int line = 0; line < lines.size(); ++line) {
    if (!lines.at(line).startsWith(prefix)) {
      continue;
    }
    bool okay = false;
    const int index = QStringView {lines.at(line)}.mid(prefix.size()).toInt(&okay);
    if (!okay || index < 0) {
      continue;
    }
    completed->insert(index);
    // Everything up to the next record marker belongs to this file. Fields
    // stay positional, so a short record simply has empty trailing fields.
    QStringList fields;
    for (int next = line + 1; next < lines.size() && !lines.at(next).startsWith(prefix); ++next) {
      fields.append(lines.at(next));
    }
    results.insert(index, parseImageDetails(fields.join(QLatin1Char('\n')).toUtf8()));
  }
  return results;
}

QDateTime MediaMetadataIndex::parseVideoDate(const QByteArray& output) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(output, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return {};
  }

  const QJsonObject root = document.object();
  QDateTime parsed = dateFromTags(
      root.value(QStringLiteral("format")).toObject().value(QStringLiteral("tags")).toObject());
  if (parsed.isValid()) {
    return parsed;
  }
  for (const auto& value : root.value(QStringLiteral("streams")).toArray()) {
    parsed = dateFromTags(value.toObject().value(QStringLiteral("tags")).toObject());
    if (parsed.isValid()) {
      return parsed;
    }
  }
  return {};
}

MediaMetadataIndex::Details MediaMetadataIndex::parseVideoDetails(const QByteArray& output) {
  Details details;
  details.captured = parseVideoDate(output);
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(output, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return details;
  }
  const QJsonObject tags =
      document.object().value(QStringLiteral("format")).toObject().value(QStringLiteral("tags")).toObject();
  details.camera = cameraName(
      tagValue(tags, {QStringLiteral("com.apple.quicktime.make"), QStringLiteral("make")}),
      tagValue(tags, {QStringLiteral("com.apple.quicktime.model"), QStringLiteral("model")}));
  return details;
}

CaptureModel::MetadataUpdate MediaMetadataIndex::updateFor(const Candidate& candidate,
                                                           const Details& details) {
  return {candidate.path,   candidate.modified, candidate.bytes, details.captured,
          candidate.device, candidate.inode,    details.camera,  details.lens};
}

void MediaMetadataIndex::sync() {
  if (!m_model) {
    return;
  }

  QList<CaptureModel::MetadataUpdate> cachedUpdates;
  m_queue.clear();
  // Group images so one ImageMagick process can inspect a bounded batch.
  // Each group remains newest-first in the model's scan order.
  for (const bool videos : {false, true}) {
    for (int row = 0; row < m_model->rowCount(); ++row) {
      const CaptureRecord& record = m_model->recordAt(row);
      if (record.isDocument() || record.isVideo() != videos || record.hasProducerTimestamp ||
          (videos ? m_videoProgram.isEmpty() : m_imageProgram.isEmpty())) {
        continue;
      }

      const Candidate candidate {record.path,      record.modified, record.bytes,
                                 record.isVideo(), record.device,   record.inode};
      const auto known = m_entries.constFind(record.path);
      if (known != m_entries.cend() && known->modified == record.modified &&
          known->bytes == record.bytes && known->device == record.device &&
          known->inode == record.inode) {
        if (known->details != Details {}) {
          cachedUpdates.append(updateFor(candidate, known->details));
        }
        continue;
      }
      if (known != m_entries.cend()) {
        m_entries.erase(known);
        m_cacheDirty = true;
      }

      const auto failed = m_failed.constFind(record.path);
      if ((failed != m_failed.cend() &&
           failed.value() ==
               identityKey(record.modified, record.bytes, record.device, record.inode)) ||
          isCurrent(candidate)) {
        continue;
      }
      m_queue.append(candidate);
    }
  }

  if (m_indexing || !m_current.isEmpty() || !m_pendingUpdates.isEmpty()) {
    for (const CaptureModel::MetadataUpdate& update : std::as_const(cachedUpdates)) {
      m_pendingUpdates.insert(update.path, update);
    }
  } else {
    m_model->applyMetadata(cachedUpdates);
  }
  resetProgress(m_queue.size() + m_current.size());
  setIndexing(!m_current.isEmpty() || !m_queue.isEmpty());
  if (m_cacheDirty) {
    scheduleSave();
  }
  processNext();
}

void MediaMetadataIndex::processNext() {
  if (m_process.state() != QProcess::NotRunning || !m_current.isEmpty()) {
    return;
  }
  if (m_queue.isEmpty()) {
    if (!m_pendingUpdates.isEmpty()) {
      m_model->applyMetadata(m_pendingUpdates.values());
      m_pendingUpdates.clear();
    }
    setIndexing(false);
    return;
  }

  const Candidate first = m_queue.takeFirst();
  if (!stillCurrent(first)) {
    advanceProgress();
    QTimer::singleShot(0, this, &MediaMetadataIndex::processNext);
    return;
  }

  m_current.append(first);
  if (!first.video) {
    while (m_current.size() < kImageBatchSize && !m_queue.isEmpty() && !m_queue.first().video) {
      const Candidate candidate = m_queue.takeFirst();
      if (stillCurrent(candidate)) {
        m_current.append(candidate);
      } else {
        advanceProgress();
      }
    }
  }

  m_process.setProgram(first.video ? m_videoProgram : m_imageProgram);
  m_process.setProcessChannelMode(QProcess::SeparateChannels);
  if (first.video) {
    m_process.setArguments(
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
         QStringLiteral("format_tags=creation_time,com.apple.quicktime.creationdate,"
                        "com.apple.quicktime.make,com.apple.quicktime.model,make,model:"
                        "stream_tags=creation_time,com.apple.quicktime.creationdate"),
         QStringLiteral("-of"), QStringLiteral("json"), first.path});
    m_timeout.setInterval(kVideoProbeTimeoutMs);
  } else {
    QStringList arguments = {QStringLiteral("identify"), QStringLiteral("-ping"),
                             QStringLiteral("-quiet")};
    for (int index = 0; index < m_current.size(); ++index) {
      arguments.append({QStringLiteral("-format"),
                        QStringLiteral("%1%2\\n%[EXIF:DateTimeOriginal]\\n"
                                       "%[EXIF:DateTimeDigitized]\\n%[EXIF:Make]\\n"
                                       "%[EXIF:Model]\\n%[EXIF:LensModel]\\n")
                            .arg(QString::fromLatin1(kImageRecordPrefix))
                            .arg(index),
                        m_current.at(index).path});
    }
    m_process.setArguments(arguments);
    m_timeout.setInterval(kImageBatchTimeoutMs);
  }
  m_process.start(QIODevice::ReadOnly);
  m_timeout.start();
}

void MediaMetadataIndex::finishCurrent(bool successful) {
  if (m_current.isEmpty()) {
    return;
  }
  m_timeout.stop();
  const QList<Candidate> batch = std::exchange(m_current, {});
  const QByteArray output = m_process.readAllStandardOutput();

  if (batch.first().video) {
    const Candidate& candidate = batch.first();
    if (successful && stillCurrent(candidate)) {
      adopt(candidate, parseVideoDetails(output));
    } else if (stillCurrent(candidate)) {
      m_failed.insert(candidate.path, identityKey(candidate.modified, candidate.bytes,
                                                  candidate.device, candidate.inode));
    }
    advanceProgress();
  } else {
    QSet<int> completed;
    const QHash<int, Details> results = parseImageBatch(output, &completed);
    for (int index = 0; index < batch.size(); ++index) {
      const Candidate& candidate = batch.at(index);
      if (completed.contains(index) && stillCurrent(candidate)) {
        adopt(candidate, results.value(index));
      } else if (stillCurrent(candidate)) {
        m_failed.insert(candidate.path, identityKey(candidate.modified, candidate.bytes,
                                                    candidate.device, candidate.inode));
      }
    }
    advanceProgress(static_cast<int>(batch.size()));
  }
  QTimer::singleShot(0, this, &MediaMetadataIndex::processNext);
}

bool MediaMetadataIndex::isCurrent(const Candidate& candidate) const {
  return std::any_of(m_current.cbegin(), m_current.cend(), [&candidate](const Candidate& current) {
    return current.path == candidate.path && current.modified == candidate.modified &&
           current.bytes == candidate.bytes && current.device == candidate.device &&
           current.inode == candidate.inode && current.video == candidate.video;
  });
}

bool MediaMetadataIndex::stillCurrent(const Candidate& candidate) const {
  if (!m_model) {
    return false;
  }
  const int row = m_model->rowOf(candidate.path);
  if (row < 0) {
    return false;
  }
  const CaptureRecord& record = m_model->recordAt(row);
  return !record.hasProducerTimestamp && record.modified == candidate.modified &&
         record.bytes == candidate.bytes && record.isVideo() == candidate.video &&
         record.device == candidate.device && record.inode == candidate.inode;
}

void MediaMetadataIndex::adopt(const Candidate& candidate, const Details& details) {
  m_entries.insert(candidate.path, {candidate.modified, candidate.bytes, details, candidate.device,
                                    candidate.inode});
  m_failed.remove(candidate.path);
  m_cacheDirty = true;
  scheduleSave();
  if (details != Details {}) {
    m_pendingUpdates.insert(candidate.path, updateFor(candidate, details));
  }
}

void MediaMetadataIndex::setIndexing(bool value) {
  if (m_indexing == value) {
    return;
  }
  m_indexing = value;
  emit indexingChanged();
}

void MediaMetadataIndex::resetProgress(int total) {
  total = qMax(0, total);
  if (m_completed == 0 && m_total == total) {
    return;
  }
  m_completed = 0;
  m_total = total;
  emit progressChanged();
}

void MediaMetadataIndex::advanceProgress(int amount) {
  if (m_completed >= m_total) {
    return;
  }
  m_completed = qMin(m_total, m_completed + qMax(1, amount));
  emit progressChanged();
}

void MediaMetadataIndex::loadCache() {
  QFile file(cachePath());
  if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumCacheBytes) {
    return;
  }

  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  quint32 magic = 0;
  quint16 version = 0;
  quint32 count = 0;
  stream >> magic >> version >> count;
  if (magic != kCacheMagic || version != kCacheVersion || count > kMaximumCacheEntries) {
    return;
  }

  QHash<QString, Entry> loaded;
  loaded.reserve(count);
  for (quint32 index = 0; index < count; ++index) {
    QString path;
    Entry entry;
    stream >> path >> entry.modified >> entry.bytes >> entry.details.captured >>
        entry.details.camera >> entry.details.lens >> entry.device >> entry.inode;
    if (stream.status() != QDataStream::Ok || path.isEmpty()) {
      return;
    }
    loaded.insert(path, entry);
  }
  if (stream.atEnd()) {
    m_entries = std::move(loaded);
  }
}

void MediaMetadataIndex::scheduleSave() {
  if (!m_saveTimer.isActive()) {
    m_saveTimer.start();
  }
}

void MediaMetadataIndex::saveCache() {
  if (!m_cacheDirty) {
    return;
  }

  // Keep the live library first, then as much history as fits. The history is
  // only an optimization for a folder that returns later, so dropping it is
  // always preferable to letting a rebuildable cache grow without a bound.
  QStringList ordered;
  QSet<QString> seen;
  if (m_model) {
    ordered.reserve(qMin(m_model->rowCount(), static_cast<int>(kMaximumCacheEntries)));
    for (int row = 0; row < m_model->rowCount(); ++row) {
      const QString& path = m_model->recordAt(row).path;
      if (m_entries.contains(path) && !seen.contains(path)) {
        ordered.append(path);
        seen.insert(path);
      }
    }
  }
  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
    if (!seen.contains(it.key())) {
      ordered.append(it.key());
    }
  }

  QStringList retained;
  qint64 estimatedBytes = 64;
  for (const QString& key : std::as_const(ordered)) {
    // QDataStream stores QString as UTF-16 plus fixed-size identity fields.
    // The extra margin covers QDateTime's zone representation, the camera
    // and lens names, and framing.
    const Entry& sized = m_entries[key];
    const qint64 entryBytes =
        320 + qint64(key.size() + sized.details.camera.size() + sized.details.lens.size()) * 2;
    if (retained.size() >= static_cast<int>(kMaximumCacheEntries) ||
        estimatedBytes + entryBytes > kMaximumCacheBytes) {
      continue;
    }
    retained.append(key);
    estimatedBytes += entryBytes;
  }

  const QString path = cachePath();
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    return;
  }
  QFile::setPermissions(QFileInfo(path).absolutePath(),
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return;
  }
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << kCacheMagic << kCacheVersion << static_cast<quint32>(retained.size());
  for (const QString& key : std::as_const(retained)) {
    const Entry entry = m_entries.value(key);
    stream << key << entry.modified << entry.bytes << entry.details.captured
           << entry.details.camera << entry.details.lens << entry.device << entry.inode;
  }
  if (stream.status() == QDataStream::Ok && file.size() <= kMaximumCacheBytes && file.commit()) {
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QHash<QString, Entry> kept;
    kept.reserve(retained.size());
    for (const QString& key : std::as_const(retained)) {
      kept.insert(key, m_entries.value(key));
    }
    m_entries = std::move(kept);
    m_cacheDirty = false;
  } else {
    file.cancelWriting();
  }
}
