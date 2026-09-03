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

#include <algorithm>
#include <utility>

namespace {

constexpr quint32 kCacheMagic = 0x4f4d4431; // OMD1
constexpr quint16 kCacheVersion = 2;
constexpr qint64 kMaximumCacheBytes = 16 * 1024 * 1024;
constexpr quint32 kMaximumCacheEntries = 100'000;
constexpr int kImageBatchSize = 32;
constexpr int kImageBatchTimeoutMs = 30'000;
constexpr int kVideoProbeTimeoutMs = 5000;
constexpr auto kImageRecordPrefix = "\x1e" "OMAROLL_DATE:";

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

QHash<int, QDateTime> MediaDateIndex::parseImageBatch(const QByteArray& output,
                                                      QSet<int>* completed) {
  QHash<int, QDateTime> dates;
  const QStringList lines =
      QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  const QString prefix = QString::fromLatin1(kImageRecordPrefix);
  for (int line = 0; line < lines.size(); ++line) {
    if (!lines.at(line).startsWith(prefix)) {
      continue;
    }
    bool okay = false;
    const int index = QStringView{lines.at(line)}.mid(prefix.size()).toInt(&okay);
    if (!okay || index < 0) {
      continue;
    }
    completed->insert(index);
    const QString fields = lines.value(line + 1) + QLatin1Char('\n') +
                           lines.value(line + 2) + QLatin1Char('\n');
    dates.insert(index, parseImageDate(fields.toUtf8()));
  }
  return dates;
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
  // Group images so one ImageMagick process can inspect a bounded batch.
  // Each group remains newest-first in the model's scan order.
  for (const bool videos : {false, true}) {
    for (int row = 0; row < m_model->rowCount(); ++row) {
      const CaptureRecord& record = m_model->recordAt(row);
      if (record.isVideo() != videos || record.hasProducerTimestamp ||
          (videos ? m_videoProgram.isEmpty() : m_imageProgram.isEmpty())) {
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
          isCurrent(candidate)) {
        continue;
      }
      m_queue.append(candidate);
    }
  }

  if (m_indexing || !m_current.isEmpty() || !m_pendingDates.isEmpty()) {
    for (const CaptureModel::CapturedDateUpdate& update : std::as_const(cachedDates)) {
      m_pendingDates.insert(update.path, update);
    }
  } else {
    m_model->applyCapturedDates(cachedDates);
  }
  resetProgress(m_queue.size() + m_current.size());
  setIndexing(!m_current.isEmpty() || !m_queue.isEmpty());
  if (m_cacheDirty) {
    scheduleSave();
  }
  processNext();
}

void MediaDateIndex::processNext() {
  if (m_process.state() != QProcess::NotRunning || !m_current.isEmpty()) {
    return;
  }
  if (m_queue.isEmpty()) {
    if (!m_pendingDates.isEmpty()) {
      m_model->applyCapturedDates(m_pendingDates.values());
      m_pendingDates.clear();
    }
    setIndexing(false);
    return;
  }

  const Candidate first = m_queue.takeFirst();
  if (!stillCurrent(first)) {
    advanceProgress();
    QTimer::singleShot(0, this, &MediaDateIndex::processNext);
    return;
  }

  m_current.append(first);
  if (!first.video) {
    while (m_current.size() < kImageBatchSize && !m_queue.isEmpty() &&
           !m_queue.first().video) {
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
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-show_entries"),
         QStringLiteral("format_tags=creation_time,com.apple.quicktime.creationdate:"
                        "stream_tags=creation_time,com.apple.quicktime.creationdate"),
         QStringLiteral("-of"), QStringLiteral("json"), first.path});
    m_timeout.setInterval(kVideoProbeTimeoutMs);
  } else {
    QStringList arguments = {QStringLiteral("identify"), QStringLiteral("-ping"),
                             QStringLiteral("-quiet")};
    for (int index = 0; index < m_current.size(); ++index) {
      arguments.append(
          {QStringLiteral("-format"),
           QStringLiteral("%1%2\\n%[EXIF:DateTimeOriginal]\\n"
                          "%[EXIF:DateTimeDigitized]\\n")
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

void MediaDateIndex::finishCurrent(bool successful) {
  if (m_current.isEmpty()) {
    return;
  }
  m_timeout.stop();
  const QList<Candidate> batch = std::exchange(m_current, {});
  const QByteArray output = m_process.readAllStandardOutput();

  if (batch.first().video) {
    const Candidate& candidate = batch.first();
    if (successful && stillCurrent(candidate)) {
      adopt(candidate, parseVideoDate(output));
    } else if (stillCurrent(candidate)) {
      m_failed.insert(candidate.path,
                      identityKey(candidate.modified, candidate.bytes, candidate.device,
                                  candidate.inode));
    }
    advanceProgress();
  } else {
    QSet<int> completed;
    const QHash<int, QDateTime> dates = parseImageBatch(output, &completed);
    for (int index = 0; index < batch.size(); ++index) {
      const Candidate& candidate = batch.at(index);
      if (completed.contains(index) && stillCurrent(candidate)) {
        adopt(candidate, dates.value(index));
      } else if (stillCurrent(candidate)) {
        m_failed.insert(candidate.path,
                        identityKey(candidate.modified, candidate.bytes, candidate.device,
                                    candidate.inode));
      }
      advanceProgress();
    }
  }
  QTimer::singleShot(0, this, &MediaDateIndex::processNext);
}

bool MediaDateIndex::isCurrent(const Candidate& candidate) const {
  return std::any_of(m_current.cbegin(), m_current.cend(),
                     [&candidate](const Candidate& current) {
                       return current.path == candidate.path &&
                              current.modified == candidate.modified &&
                              current.bytes == candidate.bytes &&
                              current.device == candidate.device &&
                              current.inode == candidate.inode &&
                              current.video == candidate.video;
                     });
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
    m_pendingDates.insert(candidate.path,
                          {candidate.path, candidate.modified, candidate.bytes, captured,
                           candidate.device, candidate.inode});
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
