#include "library/MediaDateIndex.h"

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

#include <utility>

namespace {

constexpr quint32 kCacheMagic = 0x4f4d4431; // OMD1
constexpr quint16 kCacheVersion = 2;
constexpr qint64 kMaximumCacheBytes = 16 * 1024 * 1024;
constexpr quint32 kMaximumCacheEntries = 100'000;
constexpr int kProbeTimeoutMs = 5000;

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache")
                              : configured;
}

QString cleanValue(QString value) {
  value = value.trimmed();
  return value.compare(QStringLiteral("undefined"), Qt::CaseInsensitive) == 0
             ? QString()
             : value;
}

QDateTime exifDate(const QString& value) {
  const QString clean = cleanValue(value);
  for (const QString& candidate : {clean, clean.left(23), clean.left(19)}) {
    for (const QString& format : {QStringLiteral("yyyy:MM:dd HH:mm:ss.zzz"),
                                  QStringLiteral("yyyy:MM:dd HH:mm:ss"),
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
  if (parsed.isValid() &&
      (parsed.timeSpec() == Qt::UTC || parsed.timeSpec() == Qt::OffsetFromUTC ||
       parsed.timeSpec() == Qt::TimeZone)) {
    return parsed.toLocalTime();
  }
  return parsed;
}

QDateTime dateFromTags(const QJsonObject& tags) {
  for (const QString& key : {QStringLiteral("com.apple.quicktime.creationdate"),
                             QStringLiteral("creation_time")}) {
    const QDateTime parsed = isoDate(tags.value(key).toString());
    if (parsed.isValid()) {
      return parsed;
    }
  }
  return {};
}

QString identityKey(qint64 modified, qint64 bytes, quint64 device, quint64 inode) {
  return QStringLiteral("%1:%2:%3:%4")
      .arg(modified)
      .arg(bytes)
      .arg(device)
      .arg(inode);
}

} // namespace

MediaDateIndex::MediaDateIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model),
      m_imageProgram(QStandardPaths::findExecutable(QStringLiteral("magick"))),
      m_videoProgram(QStandardPaths::findExecutable(QStringLiteral("ffprobe"))) {
  loadCache();

  m_timeout.setSingleShot(true);
  m_timeout.setInterval(kProbeTimeoutMs);
  connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
  connect(&m_process, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            finishCurrent(status == QProcess::NormalExit && exitCode == 0);
          });
  connect(&m_process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
              finishCurrent(false);
            }
          });

  m_syncTimer.setSingleShot(true);
  m_syncTimer.setInterval(100);
  connect(&m_syncTimer, &QTimer::timeout, this, &MediaDateIndex::sync);

  m_saveTimer.setSingleShot(true);
  // Batch many quick metadata reads into one small atomic cache write.
  m_saveTimer.setInterval(5000);
  connect(&m_saveTimer, &QTimer::timeout, this, &MediaDateIndex::saveCache);

  if (m_model) {
    connect(m_model, &CaptureModel::countChanged, &m_syncTimer,
            qOverload<>(&QTimer::start));
    if (m_model->rowCount() > 0) {
      m_syncTimer.start();
    }
  }
}

MediaDateIndex::~MediaDateIndex() {
  disconnect(&m_process, nullptr, this, nullptr);
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
  if (m_cacheDirty) {
    saveCache();
  }
}

QString MediaDateIndex::cachePath() {
  return cacheHome() + QStringLiteral("/omaroll/media-dates.index");
}

QDateTime MediaDateIndex::parseImageDate(const QByteArray& output) {
  const QStringList fields =
      QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  for (int index = 0; index < 2; ++index) {
    const QDateTime parsed = exifDate(fields.value(index));
    if (parsed.isValid()) {
      return parsed;
    }
  }
  return {};
}

QDateTime MediaDateIndex::parseVideoDate(const QByteArray& output) {
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
  for (const QJsonValue& value : root.value(QStringLiteral("streams")).toArray()) {
    parsed = dateFromTags(value.toObject().value(QStringLiteral("tags")).toObject());
    if (parsed.isValid()) {
      return parsed;
    }
  }
  return {};
}

void MediaDateIndex::sync() {
  if (!m_model) {
    return;
  }

  QList<CaptureModel::CapturedDateUpdate> cachedDates;
  m_queue.clear();
  for (int row = 0; row < m_model->rowCount(); ++row) {
    const CaptureRecord& record = m_model->recordAt(row);
    if (record.hasProducerTimestamp ||
        (record.isVideo() ? m_videoProgram.isEmpty() : m_imageProgram.isEmpty())) {
      continue;
    }

    const Candidate candidate{record.path, record.modified, record.bytes,
                              record.isVideo(), record.device, record.inode};
    const auto known = m_entries.constFind(record.path);
    if (known != m_entries.cend() && known->modified == record.modified &&
        known->bytes == record.bytes && known->device == record.device &&
        known->inode == record.inode) {
      if (known->captured.isValid()) {
        cachedDates.append({record.path, record.modified, record.bytes,
                            known->captured, record.device, record.inode});
      }
      continue;
    }
    if (known != m_entries.cend()) {
      m_entries.erase(known);
      m_cacheDirty = true;
    }

    const auto failed = m_failed.constFind(record.path);
    if ((failed != m_failed.cend() &&
         failed.value() == identityKey(record.modified, record.bytes, record.device,
                                       record.inode)) ||
        (m_current && m_current->path == record.path &&
         m_current->modified == record.modified && m_current->bytes == record.bytes &&
         m_current->device == record.device && m_current->inode == record.inode &&
         m_current->video == record.isVideo())) {
      continue;
    }
    m_queue.append(candidate);
  }

  m_model->applyCapturedDates(cachedDates);
  resetProgress(m_queue.size() + (m_current ? 1 : 0));
  setIndexing(m_current.has_value() || !m_queue.isEmpty());
  if (m_cacheDirty) {
    scheduleSave();
  }
  processNext();
}

void MediaDateIndex::processNext() {
  if (m_process.state() != QProcess::NotRunning || m_current.has_value()) {
    return;
  }
  if (m_queue.isEmpty()) {
    setIndexing(false);
    return;
  }

  const Candidate candidate = m_queue.takeFirst();
  if (!stillCurrent(candidate)) {
    advanceProgress();
    QTimer::singleShot(0, this, &MediaDateIndex::processNext);
    return;
  }

  m_current = candidate;
  m_process.setProgram(candidate.video ? m_videoProgram : m_imageProgram);
  m_process.setProcessChannelMode(QProcess::SeparateChannels);
  if (candidate.video) {
    m_process.setArguments(
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-show_entries"),
         QStringLiteral("format_tags=creation_time,com.apple.quicktime.creationdate:"
                        "stream_tags=creation_time,com.apple.quicktime.creationdate"),
         QStringLiteral("-of"), QStringLiteral("json"), candidate.path});
  } else {
    m_process.setArguments(
        {QStringLiteral("identify"), QStringLiteral("-ping"),
         QStringLiteral("-quiet"), QStringLiteral("-format"),
         QStringLiteral("%[EXIF:DateTimeOriginal]\\n%[EXIF:DateTimeDigitized]\\n"),
         candidate.path});
  }
  m_process.start(QIODevice::ReadOnly);
  m_timeout.start();
}

void MediaDateIndex::finishCurrent(bool successful) {
  if (!m_current) {
    return;
  }
  m_timeout.stop();
  const Candidate candidate = *m_current;
  m_current.reset();

  if (successful && stillCurrent(candidate)) {
    const QByteArray output = m_process.readAllStandardOutput();
    adopt(candidate, candidate.video ? parseVideoDate(output) : parseImageDate(output));
  } else {
    m_failed.insert(candidate.path,
                    identityKey(candidate.modified, candidate.bytes, candidate.device,
                                candidate.inode));
  }
  advanceProgress();
  QTimer::singleShot(0, this, &MediaDateIndex::processNext);
}

bool MediaDateIndex::stillCurrent(const Candidate& candidate) const {
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

void MediaDateIndex::adopt(const Candidate& candidate, const QDateTime& captured) {
  m_entries.insert(candidate.path, {candidate.modified, candidate.bytes, captured,
                                    candidate.device, candidate.inode});
  m_failed.remove(candidate.path);
  m_cacheDirty = true;
  scheduleSave();
  if (captured.isValid()) {
    m_model->applyCapturedDates({{candidate.path, candidate.modified, candidate.bytes,
                                  captured, candidate.device, candidate.inode}});
  }
}

void MediaDateIndex::setIndexing(bool value) {
  if (m_indexing == value) {
    return;
  }
  m_indexing = value;
  emit indexingChanged();
}

void MediaDateIndex::resetProgress(int total) {
  total = qMax(0, total);
  if (m_completed == 0 && m_total == total) {
    return;
  }
  m_completed = 0;
  m_total = total;
  emit progressChanged();
}

void MediaDateIndex::advanceProgress() {
  if (m_completed >= m_total) {
    return;
  }
  ++m_completed;
  emit progressChanged();
}

void MediaDateIndex::loadCache() {
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
    stream >> path >> entry.modified >> entry.bytes >> entry.captured >> entry.device >> entry.inode;
    if (stream.status() != QDataStream::Ok || path.isEmpty()) {
      return;
    }
    loaded.insert(path, entry);
  }
  if (stream.atEnd()) {
    m_entries = std::move(loaded);
  }
}

void MediaDateIndex::scheduleSave() {
  if (!m_saveTimer.isActive()) {
    m_saveTimer.start();
  }
}

void MediaDateIndex::saveCache() {
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
    // The extra margin covers QDateTime's zone representation and framing.
    const qint64 entryBytes = 256 + qint64(key.size()) * 2;
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
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                            QFileDevice::ExeOwner);

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return;
  }
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << kCacheMagic << kCacheVersion << static_cast<quint32>(retained.size());
  for (const QString& key : std::as_const(retained)) {
    const Entry entry = m_entries.value(key);
    stream << key << entry.modified << entry.bytes << entry.captured << entry.device
           << entry.inode;
  }
  if (stream.status() == QDataStream::Ok && file.size() <= kMaximumCacheBytes &&
      file.commit()) {
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
