#include "library/MediaMetadataIndex.h"
#include "app/AppSettings.h"
#include "app/StartupTrace.h"
#include "app/TsanHandoff.h"

#include "sources/CameraRaw.h"

#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QtConcurrent>

#include <algorithm>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
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
// The embedded cache: keywords, rating and description per file.
constexpr quint32 kEmbeddedMagic = 0x4f4d4531; // OME1
constexpr quint16 kEmbeddedVersion = 1;
constexpr quint32 kMaximumEmbeddedEntries = 400'000;

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured;
}

QString cleanValue(QString value) {
  value = value.trimmed();
  return value.compare(QStringLiteral("undefined"), Qt::CaseInsensitive) == 0 ? QString() : value;
}

int digits(QStringView text, qsizetype from, qsizetype count) {
  int value = 0;
  for (qsizetype index = from; index < from + count; ++index) {
    const char16_t digit = text.at(index).unicode();
    if (digit < u'0' || digit > u'9') return -1;
    value = value * 10 + (digit - u'0');
  }
  return value;
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

// Probes run below the window's priority, for CPU and for disk, so a large
// first index never competes with scrolling or decoding. This is set from
// here just after the start: a child process modifier would make Qt fork the
// whole window process for every probe instead of using vfork.
void lowerPriority(qint64 pid) {
  if (pid <= 0) return;
  (void)setpriority(PRIO_PROCESS, static_cast<id_t>(pid), 10);
#ifdef SYS_ioprio_set
  constexpr int kWhoProcess = 1;
  constexpr int kClassBestEffort = 2;
  constexpr int kClassShift = 13;
  (void)syscall(SYS_ioprio_set, kWhoProcess, static_cast<int>(pid),
                (kClassBestEffort << kClassShift) | 7);
#endif
}

// Pool threads parse and read raw headers. QThread::LowPriority maps to nice 0
// under Linux's normal scheduler, so lower each worker thread the same way as
// the probes, once per thread. The pool is this index's own.
void lowerThisThread() {
  thread_local bool lowered = false;
  if (std::exchange(lowered, true)) return;
  lowerPriority(static_cast<qint64>(syscall(SYS_gettid)));
}

QString identityKey(qint64 modified, qint64 bytes, quint64 device, quint64 inode) {
  return QStringLiteral("%1:%2:%3:%4").arg(modified).arg(bytes).arg(device).arg(inode);
}

// Keywords, a rating and a description from a file and its sidecar. Counted
// for tests, and always off the window thread.
EmbeddedMetadata::Fields readEmbedded(const QString& path, const QString& sidecar) {
#ifdef OMAROLL_TESTING
  MediaMetadataIndex::s_embeddedReads.fetch_add(1);
  if (QThread::isMainThread()) MediaMetadataIndex::s_embeddedReadsOnWindowThread.fetch_add(1);
#endif
  return EmbeddedMetadata::read(path, sidecar);
}

// The per-file limits of the embedded cache.
EmbeddedMetadata::Fields bounded(EmbeddedMetadata::Fields fields) {
  if (fields.keywords.size() > EmbeddedMetadata::kMaximumKeywords)
    fields.keywords = fields.keywords.first(EmbeddedMetadata::kMaximumKeywords);
  fields.keywords.removeIf([](const QString& keyword) {
    return keyword.isEmpty() || keyword.size() > EmbeddedMetadata::kMaximumKeywordLength;
  });
  fields.description = fields.description.left(2000);
  if (fields.rating < 0 || fields.rating > 5) fields.rating = 0;
  return fields;
}

} // namespace

#ifdef OMAROLL_TESTING
std::atomic<int> MediaMetadataIndex::s_embeddedReadsOnWindowThread{0};
std::atomic<int> MediaMetadataIndex::s_embeddedReads{0};
#endif

struct MediaMetadataIndex::Probe {
  // Embedded reads only a file's own data, in-process: for files whose date
  // is already known, whose sidecar changed, or without ImageMagick.
  enum class Kind { Image, Video, Raw, Embedded };
  QList<Candidate> batch;
  Kind kind = Kind::Image;
  // Removed from the library while running: the result is discarded and the
  // process is stopped early.
  bool cancelled = false;
  // The same, for a raw probe's worker, which stops between files.
  std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);
  bool parsing = false;
  bool finished = false;
  // Index in the batch, path and sidecar of each file whose own data is read
  // by this probe's worker. Shared read-only with it.
  std::shared_ptr<const QList<std::tuple<int, QString, QString>>> embeddedJobs =
      std::make_shared<const QList<std::tuple<int, QString, QString>>>();
  std::unique_ptr<QProcess> process;
  QTimer timeout;
  QFutureWatcher<Parsed> parse;
};

int MediaMetadataIndex::maximumParallelProbes() {
  return std::clamp(QThread::idealThreadCount() / 2, 2, 4);
}

QDateTime MediaMetadataIndex::parseExifDateGeneral(const QString& value) {
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

QDateTime MediaMetadataIndex::parseExifDate(const QString& value) {
  // QDateTime::fromString resolves the local zone for every format it tries,
  // which made date parsing most of a large first index. EXIF dates almost
  // always have exactly one of these shapes, so read their fields directly.
  // Anything else, and any local time a zone transition skips, goes through
  // the general parser, so the result never differs from it.
  const QString clean = cleanValue(value);
  if (!clean.contains(QLatin1Char(':'))) {
    return {}; // Every accepted format has a time with colons.
  }
  const qsizetype size = clean.size();
  const QChar dateSeparator = size >= 19 ? clean.at(4) : QChar();
  const bool shaped =
      (size == 19 || (size == 23 && dateSeparator == QLatin1Char(':') &&
                      clean.at(19) == QLatin1Char('.'))) &&
      (dateSeparator == QLatin1Char(':') || dateSeparator == QLatin1Char('-')) &&
      clean.at(7) == dateSeparator && clean.at(10) == QLatin1Char(' ') &&
      clean.at(13) == QLatin1Char(':') && clean.at(16) == QLatin1Char(':');
  if (shaped) {
    const int year = digits(clean, 0, 4);
    const int month = digits(clean, 5, 2);
    const int day = digits(clean, 8, 2);
    const int hour = digits(clean, 11, 2);
    const int minute = digits(clean, 14, 2);
    const int second = digits(clean, 17, 2);
    const int millisecond = size == 23 ? digits(clean, 20, 3) : 0;
    if (year > 0 && month >= 0 && day >= 0 && hour >= 0 && minute >= 0 && second >= 0 &&
        millisecond >= 0) {
      const QDate date(year, month, day);
      const QTime time(hour, minute, second, millisecond);
      if (date.isValid() && time.isValid()) {
        const QDateTime parsed(date, time);
        if (parsed.isValid() && parsed.date() == date && parsed.time() == time) {
          return parsed;
        }
      }
    }
  }
  return parseExifDateGeneral(value);
}

MediaMetadataIndex::MediaMetadataIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model),
      m_imageProgram(QStandardPaths::findExecutable(QStringLiteral("magick"))),
      m_videoProgram(QStandardPaths::findExecutable(QStringLiteral("ffprobe"))) {
  loadCache();
  loadEmbeddedCache();

  m_embeddedTimer.setSingleShot(true);
  m_embeddedTimer.setInterval(kEmbeddedIntervalMs);
  connect(&m_embeddedTimer, &QTimer::timeout, this, &MediaMetadataIndex::flushEmbedded);
  if (AppSettings* settings = m_model ? m_model->settings() : nullptr) {
    connect(settings, &AppSettings::readEmbeddedMetadataChanged, this, [this, settings] {
      if (!settings->readEmbeddedMetadata()) {
        m_pendingEmbedded.clear();
        return;
      }
      seedEmbedded();
      m_syncTimer.start();
    });
    seedEmbedded();
  }

  m_parallel = maximumParallelProbes();
  m_pool.setMaxThreadCount(m_parallel);

  m_progressTimer.setSingleShot(true);
  m_progressTimer.setInterval(kProgressIntervalMs);
  connect(&m_progressTimer, &QTimer::timeout, this, [this] {
    if (std::exchange(m_progressPending, false)) publishProgress();
  });

  m_syncTimer.setSingleShot(true);
  m_syncTimer.setInterval(100);
  connect(&m_syncTimer, &QTimer::timeout, this, &MediaMetadataIndex::sync);

  m_updateTimer.setSingleShot(true);
  m_updateTimer.setInterval(300);
  connect(&m_updateTimer, &QTimer::timeout, this, &MediaMetadataIndex::flushUpdates);

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
  requestStop();
  for (const auto& probe : m_probes) {
    if (probe->process && probe->process->state() != QProcess::NotRunning) {
      probe->process->waitForFinished(1000);
    }
  }
  // Parsing captures only its own copies; raw reads stop at the next file.
  m_pool.waitForDone();
  if (m_cacheDirty || m_embeddedDirty) {
    saveCache();
  }
}

void MediaMetadataIndex::requestStop() {
  m_stopping->store(true);
  m_syncTimer.stop();
  m_updateTimer.stop();
  m_saveTimer.stop();
  m_progressTimer.stop();
  m_embeddedTimer.stop();
  m_queue.clear();
  for (const auto& probe : m_probes) {
    probe->timeout.stop();
    disconnect(&probe->parse, nullptr, this, nullptr);
    if (probe->process) {
      disconnect(probe->process.get(), nullptr, this, nullptr);
      if (probe->process->state() != QProcess::NotRunning) probe->process->kill();
    }
  }
}

QString MediaMetadataIndex::cachePath() {
  return cacheHome() + QStringLiteral("/omaroll/media-metadata.index");
}

QString MediaMetadataIndex::embeddedCachePath() {
  return cacheHome() + QStringLiteral("/omaroll/embedded-v1.index");
}

QDateTime MediaMetadataIndex::parseImageDate(const QByteArray& output) {
  const QStringList fields = QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  for (int index = 0; index < 2; ++index) {
    const QDateTime parsed = parseExifDate(fields.value(index));
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

MediaMetadataIndex::Details MediaMetadataIndex::rawDetails(const QString& path) {
  const CameraRaw::Metadata metadata = CameraRaw::readMetadata(path);
  Details details;
  details.captured = metadata.taken;
  details.camera = cameraName(metadata.make, metadata.model);
  details.lens = metadata.lens.simplified();
  return details;
}

CaptureModel::MetadataUpdate MediaMetadataIndex::updateFor(const Candidate& candidate,
                                                           const Details& details) {
  return {candidate.path,   candidate.modified, candidate.bytes, details.captured,
          candidate.device, candidate.inode,    details.camera,  details.lens};
}

bool MediaMetadataIndex::readsEmbedded() const {
  const AppSettings* settings = m_model ? m_model->settings() : nullptr;
  return settings && settings->readEmbeddedMetadata();
}

bool MediaMetadataIndex::embeddedMatches(const EmbeddedEntry& entry, const CaptureRecord& record) const {
  return entry.modified == record.modified && entry.bytes == record.bytes &&
         entry.device == record.device && entry.inode == record.inode &&
         entry.sidecar == record.sidecarPath && entry.sidecarModified == record.sidecarModified &&
         entry.sidecarBytes == record.sidecarBytes;
}

void MediaMetadataIndex::sync() {
  if (m_stopping->load() || !m_model) {
    return;
  }

  const bool embedded = readsEmbedded();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  QList<CaptureModel::MetadataUpdate> cachedUpdates;
  QList<Candidate> embeddedOnly;
  QSet<QString> live;
  live.reserve(m_model->rowCount());
  m_queue.clear();
  // Group images so one ImageMagick process can inspect a bounded batch.
  // Each group remains newest-first in the model's scan order.
  for (const bool videos : {false, true}) {
    for (int row = 0; row < m_model->rowCount(); ++row) {
      const CaptureRecord& record = m_model->recordAt(row);
      if (!videos) live.insert(record.path);
      if (record.isDocument() || record.isVideo() != videos || record.hasProducerTimestamp) {
        continue;
      }

      Candidate candidate {record.path,      record.modified, record.bytes,
                           record.isVideo(), record.device,   record.inode,
                           record.isRaw()};
      candidate.sidecar = record.sidecarPath;
      candidate.sidecarModified = record.sidecarModified;
      candidate.sidecarBytes = record.sidecarBytes;

      // Keywords, rating and description: read once per file and sidecar
      // version. A video has only its sidecar.
      bool needsEmbedded = false;
      if (embedded) {
        const auto known = m_embedded.find(record.path);
        if (known != m_embedded.end() && embeddedMatches(*known, record)) {
          known->seen = now;
          if (!known->fields.isEmpty()) m_pendingEmbedded.insert(record.path, *known);
        } else if (record.isVideo() && record.sidecarPath.isEmpty()) {
          if (known != m_embedded.end()) {
            // Its sidecar went away.
            EmbeddedEntry gone = *known;
            gone.fields = {};
            m_embedded.erase(known);
            m_embeddedDirty = true;
            m_pendingEmbedded.insert(record.path, gone);
          }
        } else {
          needsEmbedded = true;
        }
      }

      bool needsDate = false;
      if (videos ? !m_videoProgram.isEmpty() : !m_imageProgram.isEmpty() || record.isRaw()) {
        const auto known = m_entries.constFind(record.path);
        if (known != m_entries.cend() && known->modified == record.modified &&
            known->bytes == record.bytes && known->device == record.device &&
            known->inode == record.inode) {
          if (known->details != Details {}) {
            cachedUpdates.append(updateFor(candidate, known->details));
          }
        } else {
          if (known != m_entries.cend()) {
            m_entries.erase(known);
            m_cacheDirty = true;
          }
          const auto failed = m_failed.constFind(record.path);
          needsDate = failed == m_failed.cend() ||
                      failed.value() !=
                          identityKey(record.modified, record.bytes, record.device, record.inode);
        }
      }

      if (!needsDate && !needsEmbedded) continue;
      candidate.needsDate = needsDate;
      candidate.needsEmbedded = needsEmbedded;
      if (isCurrent(candidate)) continue;
      // A file whose date is known only needs its own data read, in-process.
      (needsDate ? m_queue : embeddedOnly).append(candidate);
    }
  }
  m_queue.append(embeddedOnly);

  for (const CaptureModel::MetadataUpdate& update : std::as_const(cachedUpdates)) {
    m_pendingUpdates.insert(update.path, update);
  }
  if (!m_pendingUpdates.isEmpty() && !m_updateTimer.isActive()) m_updateTimer.start();
  if (embedded) {
    // Values for files that left the library go, then the cached ones for
    // what is here arrive in one batch.
    m_model->settings()->retainEmbedded(live);
    flushEmbedded();
  }
  cancelStaleProbes();
  resetProgress(static_cast<int>(m_queue.size()) + inFlightCount());
  setIndexing(inFlightCount() > 0 || !m_queue.isEmpty());
  if (m_cacheDirty || m_embeddedDirty) {
    scheduleSave();
  }
  processNext();
}

void MediaMetadataIndex::processNext() {
  if (m_stopping->load()) return;
  std::erase_if(m_probes, [](const auto& probe) { return probe->finished; });
  while (static_cast<int>(m_probes.size()) < m_parallel && startProbe()) {
  }
  if (m_probes.empty() && m_queue.isEmpty()) {
    flushUpdates();
    flushEmbedded();
    setIndexing(false);
    if (std::exchange(m_progressPending, false)) publishProgress();
  }
}

bool MediaMetadataIndex::startProbe() {
  QList<Candidate> batch;
  while (batch.isEmpty() && !m_queue.isEmpty()) {
    const Candidate first = m_queue.takeFirst();
    if (stillCurrent(first)) {
      batch.append(first);
    } else {
      advanceProgress();
    }
  }
  if (batch.isEmpty()) {
    return false;
  }

  // A copy: the batch grows below.
  const Candidate first = batch.first();
  auto probe = std::make_unique<Probe>();
  probe->kind = !first.needsDate ? Probe::Kind::Embedded
                : first.raw      ? Probe::Kind::Raw
                : first.video    ? Probe::Kind::Video
                                 : Probe::Kind::Image;
  if (probe->kind == Probe::Kind::Embedded) {
    while (batch.size() < kEmbeddedBatchSize && !m_queue.isEmpty() && !m_queue.first().needsDate) {
      const Candidate candidate = m_queue.takeFirst();
      if (stillCurrent(candidate)) {
        batch.append(candidate);
      } else {
        advanceProgress();
      }
    }
  } else if (probe->kind != Probe::Kind::Video) {
    // Raws are a few header reads each; images share one ImageMagick run.
    while (batch.size() < kImageBatchSize && !m_queue.isEmpty() && !m_queue.first().video &&
           m_queue.first().needsDate && m_queue.first().raw == first.raw) {
      const Candidate candidate = m_queue.takeFirst();
      if (stillCurrent(candidate)) {
        batch.append(candidate);
      } else {
        advanceProgress();
      }
    }
  }
  probe->batch = std::move(batch);
  Probe* raw = probe.get();
  connect(&raw->parse, &QFutureWatcher<Parsed>::finished, this, [this, raw] {
    // Leave the watcher's own signal before the probe can be released.
    QMetaObject::invokeMethod(
        this, [this, raw] { finishProbe(raw, raw->parse.future().result()); },
        Qt::QueuedConnection);
  });

  // What each file's worker reads besides its date: its own keywords,
  // rating and description, and its sidecar's.
  {
    QList<std::tuple<int, QString, QString>> jobs;
    for (int index = 0; index < raw->batch.size(); ++index) {
      const Candidate& candidate = raw->batch.at(index);
      if (candidate.needsEmbedded) jobs.append({index, candidate.path, candidate.sidecar});
    }
    raw->embeddedJobs = std::make_shared<const QList<std::tuple<int, QString, QString>>>(std::move(jobs));
  }

  if (raw->kind == Probe::Kind::Raw || raw->kind == Probe::Kind::Embedded) {
    QStringList paths;
    for (const Candidate& candidate : std::as_const(raw->batch)) paths.append(candidate.path);
    const auto stopping = m_stopping;
    const auto stop = raw->stop;
    const bool dates = raw->kind == Probe::Kind::Raw;
    const auto jobs = raw->embeddedJobs;
    raw->parse.setFuture(QtConcurrent::run(&m_pool, [paths, stopping, stop, dates, jobs] {
      lowerThisThread();
      Parsed parsed;
      for (int index = 0; dates && index < paths.size(); ++index) {
        if (stopping->load() || stop->load()) break;
        parsed.results.insert(index, rawDetails(paths.at(index)));
        parsed.completed.insert(index);
      }
      for (const auto& [index, path, sidecar] : *jobs) {
        if (stopping->load() || stop->load()) break;
        parsed.embedded.insert(index, bounded(readEmbedded(path, sidecar)));
      }
      return parsed;
    }));
    m_probes.push_back(std::move(probe));
    return true;
  }

  raw->process = std::make_unique<QProcess>();
  QProcess* process = raw->process.get();
  process->setProgram(raw->kind == Probe::Kind::Video ? m_videoProgram : m_imageProgram);
  process->setProcessChannelMode(QProcess::SeparateChannels);
  if (raw->kind == Probe::Kind::Video) {
    process->setArguments(
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
         QStringLiteral("format_tags=creation_time,com.apple.quicktime.creationdate,"
                        "com.apple.quicktime.make,com.apple.quicktime.model,make,model:"
                        "stream_tags=creation_time,com.apple.quicktime.creationdate"),
         QStringLiteral("-of"), QStringLiteral("json"), first.path});
    raw->timeout.setInterval(kVideoProbeTimeoutMs);
  } else {
    QStringList arguments = {QStringLiteral("identify"), QStringLiteral("-ping"),
                             QStringLiteral("-quiet")};
    for (int index = 0; index < raw->batch.size(); ++index) {
      arguments.append({QStringLiteral("-format"),
                        QStringLiteral("%1%2\\n%[EXIF:DateTimeOriginal]\\n"
                                       "%[EXIF:DateTimeDigitized]\\n%[EXIF:Make]\\n"
                                       "%[EXIF:Model]\\n%[EXIF:LensModel]\\n")
                            .arg(QString::fromLatin1(kImageRecordPrefix))
                            .arg(index),
                        raw->batch.at(index).path});
    }
    process->setArguments(arguments);
    raw->timeout.setInterval(kImageBatchTimeoutMs);
  }
  raw->timeout.setSingleShot(true);
  connect(&raw->timeout, &QTimer::timeout, process, &QProcess::kill);
  connect(process, &QProcess::finished, this, [this, raw](int exitCode, QProcess::ExitStatus status) {
    processFinished(raw, status == QProcess::NormalExit && exitCode == 0);
  });
  connect(process, &QProcess::errorOccurred, this, [this, raw](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) processFinished(raw, false);
  });
  m_probes.push_back(std::move(probe));
  process->start(QIODevice::ReadOnly);
  if (process->state() != QProcess::NotRunning) {
    lowerPriority(process->processId());
    raw->timeout.start();
  }
  return true;
}

void MediaMetadataIndex::processFinished(Probe* probe, bool successful) {
  if (probe->parsing || probe->finished) {
    return; // A failed start can also report finished.
  }
  probe->parsing = true;
  probe->timeout.stop();
  const QByteArray output = probe->process->readAllStandardOutput();
  if (probe->cancelled) {
    finishProbe(probe, {});
    return;
  }
  const bool video = probe->kind == Probe::Kind::Video;
  const auto jobs = probe->embeddedJobs;
  const auto stopping = m_stopping;
  const auto stop = probe->stop;
  // Only the copy of the output crosses to the worker.
  tsan::handOver(output.constData());
  probe->parse.setFuture(QtConcurrent::run(&m_pool, [output, video, successful, jobs, stopping, stop] {
    tsan::takeOver(output.constData());
    lowerThisThread();
    Parsed parsed;
    if (video) {
      if (successful) {
        parsed.results.insert(0, parseVideoDetails(output));
        parsed.completed.insert(0);
      }
    } else {
      // A batch can fail as a whole and still have read most of its files.
      parsed.results = parseImageBatch(output, &parsed.completed);
    }
    // A file ImageMagick could not read still has its own data read here.
    for (const auto& [index, path, sidecar] : *jobs) {
      if (stopping->load() || stop->load()) break;
      parsed.embedded.insert(index, bounded(readEmbedded(path, sidecar)));
    }
    return parsed;
  }));
}

void MediaMetadataIndex::finishProbe(Probe* probe, const Parsed& parsed) {
  if (probe->finished) return;
  probe->finished = true;
  probe->timeout.stop();
  if (m_stopping->load()) return;
  if (!probe->cancelled) {
    for (int index = 0; index < probe->batch.size(); ++index) {
      const Candidate& candidate = probe->batch.at(index);
      if (!stillCurrent(candidate)) {
        continue;
      }
      if (probe->kind == Probe::Kind::Embedded) {
        // No date was asked of this probe.
      } else if (parsed.completed.contains(index)) {
        adopt(candidate, parsed.results.value(index));
      } else if (probe->kind != Probe::Kind::Raw) {
        m_failed.insert(candidate.path, identityKey(candidate.modified, candidate.bytes,
                                                    candidate.device, candidate.inode));
      }
      if (candidate.needsEmbedded && parsed.embedded.contains(index) &&
          embeddedStillCurrent(candidate)) {
        adoptEmbedded(candidate, parsed.embedded.value(index));
      }
    }
    advanceProgress(static_cast<int>(probe->batch.size()));
  }
  // Released on the next turn, outside this probe's own signals.
  QTimer::singleShot(0, this, &MediaMetadataIndex::processNext);
}

void MediaMetadataIndex::cancelStaleProbes() {
  // A probe whose files all left the library, for instance a removed folder,
  // stops now rather than reading files nobody will see.
  for (const auto& probe : m_probes) {
    if (probe->cancelled || probe->finished ||
        std::any_of(probe->batch.cbegin(), probe->batch.cend(),
                    [this](const Candidate& candidate) { return stillCurrent(candidate); })) {
      continue;
    }
    probe->cancelled = true;
    probe->stop->store(true);
    if (probe->process && probe->process->state() != QProcess::NotRunning) {
      probe->process->kill();
    }
  }
}

int MediaMetadataIndex::inFlightCount() const {
  int count = 0;
  for (const auto& probe : m_probes) {
    if (!probe->cancelled && !probe->finished) count += static_cast<int>(probe->batch.size());
  }
  return count;
}

bool MediaMetadataIndex::isCurrent(const Candidate& candidate) const {
  for (const auto& probe : m_probes) {
    if (probe->cancelled || probe->finished) continue;
    for (const Candidate& current : probe->batch) {
      // In flight already, reading at least what is needed now.
      if (current.path == candidate.path && current.modified == candidate.modified &&
          current.bytes == candidate.bytes && current.device == candidate.device &&
          current.inode == candidate.inode && current.video == candidate.video &&
          (current.needsDate || !candidate.needsDate) &&
          (!candidate.needsEmbedded ||
           (current.needsEmbedded && current.sidecar == candidate.sidecar &&
            current.sidecarModified == candidate.sidecarModified &&
            current.sidecarBytes == candidate.sidecarBytes))) {
        return true;
      }
    }
  }
  return false;
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

bool MediaMetadataIndex::embeddedStillCurrent(const Candidate& candidate) const {
  if (!stillCurrent(candidate)) return false;
  const CaptureRecord& record = m_model->recordAt(m_model->rowOf(candidate.path));
  return record.sidecarPath == candidate.sidecar && record.sidecarModified == candidate.sidecarModified &&
         record.sidecarBytes == candidate.sidecarBytes;
}

void MediaMetadataIndex::adoptEmbedded(const Candidate& candidate, const EmbeddedMetadata::Fields& fields) {
  EmbeddedEntry entry {candidate.modified, candidate.bytes,           candidate.device,
                       candidate.inode,    candidate.sidecar,         candidate.sidecarModified,
                       candidate.sidecarBytes, fields, QDateTime::currentMSecsSinceEpoch()};
  m_embedded.insert(candidate.path, entry);
  m_embeddedDirty = true;
  scheduleSave();
  if (!readsEmbedded()) return;
  // Delivered at most once a second while indexing, and once more at the end.
  m_pendingEmbedded.insert(candidate.path, entry);
  if (!m_embeddedTimer.isActive()) m_embeddedTimer.start();
}

void MediaMetadataIndex::flushEmbedded() {
  m_embeddedTimer.stop();
  const auto pending = std::exchange(m_pendingEmbedded, {});
  AppSettings* settings = m_model ? m_model->settings() : nullptr;
  if (pending.isEmpty() || !settings || !settings->readEmbeddedMetadata()) return;
  QList<AppSettings::EmbeddedUpdate> batch;
  batch.reserve(pending.size());
  for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
    const EmbeddedEntry& entry = it.value();
    batch.append({it.key(),
                  {entry.fields.keywords, entry.fields.rating, entry.fields.description, entry.bytes,
                   entry.modified, entry.device, entry.inode}});
  }
  settings->setEmbedded(batch);
}

void MediaMetadataIndex::seedEmbedded() {
  if (!readsEmbedded() || m_embedded.isEmpty()) return;
  // Only files under the folders scanned now; the first scan then keeps
  // what is still there and the index reads what changed.
  QStringList roots = m_model->rootPaths();
  for (QString& root : roots) {
    if (!root.endsWith(QLatin1Char('/'))) root.append(QLatin1Char('/'));
  }
  for (auto it = m_embedded.cbegin(); it != m_embedded.cend(); ++it) {
    if (it->fields.isEmpty()) continue;
    const QString& path = it.key();
    if (std::any_of(roots.cbegin(), roots.cend(), [&path](const QString& root) {
          return path.startsWith(root) || path + QLatin1Char('/') == root;
        })) {
      m_pendingEmbedded.insert(path, it.value());
    }
  }
  flushEmbedded();
}

void MediaMetadataIndex::adopt(const Candidate& candidate, const Details& details) {
  m_entries.insert(candidate.path, {candidate.modified, candidate.bytes, details, candidate.device,
                                    candidate.inode});
  m_failed.remove(candidate.path);
  m_cacheDirty = true;
  scheduleSave();
  if (details != Details {}) {
    m_pendingUpdates.insert(candidate.path, updateFor(candidate, details));
    if (!m_updateTimer.isActive()) m_updateTimer.start();
  }
}

void MediaMetadataIndex::flushUpdates() {
  m_updateTimer.stop();
  const auto updates = std::exchange(m_pendingUpdates, {});
  if (m_model && !updates.isEmpty()) m_model->applyMetadata(updates.values());
}

void MediaMetadataIndex::setIndexing(bool value) {
  if (m_indexing == value) {
    return;
  }
  m_indexing = value;
  StartupTrace::note(value ? "index_start" : "index_done");
  if (m_model) m_model->setMetadataIndexing(value);
  emit indexingChanged();
}

void MediaMetadataIndex::resetProgress(int total) {
  total = qMax(0, total);
  if (m_completed == 0 && m_total == total) {
    return;
  }
  m_completed = 0;
  m_total = total;
  m_progressPending = false;
  publishProgress();
}

void MediaMetadataIndex::advanceProgress(int amount) {
  if (m_completed >= m_total) {
    return;
  }
  m_completed = qMin(m_total, m_completed + qMax(1, amount));
  // Several probes finish every second on a large library. The count is
  // published about once a second, and always when it reaches the total.
  if (m_completed < m_total && m_progressTimer.isActive()) {
    m_progressPending = true;
    return;
  }
  m_progressPending = false;
  publishProgress();
}

void MediaMetadataIndex::publishProgress() {
  m_progressTimer.start();
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

void MediaMetadataIndex::loadEmbeddedCache() {
  QFile file(embeddedCachePath());
  if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumEmbeddedCacheBytes) {
    return;
  }
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  quint32 magic = 0;
  quint16 version = 0;
  quint32 count = 0;
  stream >> magic >> version >> count;
  if (magic != kEmbeddedMagic || version != kEmbeddedVersion || count > kMaximumEmbeddedEntries) {
    return;
  }
  QHash<QString, EmbeddedEntry> loaded;
  loaded.reserve(count);
  for (quint32 index = 0; index < count; ++index) {
    QString path;
    EmbeddedEntry entry;
    qint8 rating = 0;
    stream >> path >> entry.modified >> entry.bytes >> entry.device >> entry.inode >> entry.sidecar >>
        entry.sidecarModified >> entry.sidecarBytes >> entry.fields.keywords >> rating >>
        entry.fields.description >> entry.fields.hasXmp >> entry.seen;
    if (stream.status() != QDataStream::Ok || path.isEmpty()) {
      return;
    }
    entry.fields.rating = rating;
    entry.fields = bounded(entry.fields);
    loaded.insert(path, entry);
  }
  if (stream.atEnd()) {
    m_embedded = std::move(loaded);
  }
}

void MediaMetadataIndex::saveEmbeddedCache() {
  const auto entryBytes = [](const QString& path, const EmbeddedEntry& entry, bool description) {
    // UTF-16 strings with their lengths, the fixed fields and framing.
    qint64 bytes = 96 + qint64(path.size() + entry.sidecar.size()) * 2 + 8;
    for (const QString& keyword : entry.fields.keywords) bytes += 4 + qint64(keyword.size()) * 2;
    if (description) bytes += qint64(entry.fields.description.size()) * 2;
    return bytes;
  };
  // Oldest seen first, so the cap drops what has been away longest.
  QList<std::pair<qint64, QString>> seen;
  seen.reserve(m_embedded.size());
  for (auto it = m_embedded.cbegin(); it != m_embedded.cend(); ++it) seen.append({it->seen, it.key()});
  std::sort(seen.begin(), seen.end());
  QStringList bySeen;
  bySeen.reserve(seen.size());
  for (const auto& entry : std::as_const(seen)) bySeen.append(entry.second);
  qint64 total = 64;
  for (auto it = m_embedded.cbegin(); it != m_embedded.cend(); ++it) total += entryBytes(it.key(), *it, true);
  int droppedDescriptions = 0;
  int droppedEntries = 0;
  QHash<QString, EmbeddedEntry> kept = m_embedded;
  // Over the cap: descriptions go first, then whole entries, oldest seen
  // first each time. The date cache is a separate file and never waits on this.
  for (qsizetype at = 0; total > kMaximumEmbeddedCacheBytes && at < bySeen.size(); ++at) {
    EmbeddedEntry& entry = kept[bySeen.at(at)];
    if (entry.fields.description.isEmpty()) continue;
    total -= qint64(entry.fields.description.size()) * 2;
    entry.fields.description.clear();
    ++droppedDescriptions;
  }
  for (qsizetype at = 0; (total > kMaximumEmbeddedCacheBytes || kept.size() > qsizetype(kMaximumEmbeddedEntries)) &&
                         at < bySeen.size();
       ++at) {
    const auto found = kept.constFind(bySeen.at(at));
    total -= entryBytes(found.key(), *found, false);
    kept.erase(found);
    ++droppedEntries;
  }
  if (droppedDescriptions > 0 || droppedEntries > 0) {
    qDebug().noquote() << QStringLiteral("Embedded metadata cache over %1 MiB: left out %2 descriptions and %3 files")
                              .arg(kMaximumEmbeddedCacheBytes / (1024 * 1024))
                              .arg(droppedDescriptions)
                              .arg(droppedEntries);
  }

  const QString path = embeddedCachePath();
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
  stream << kEmbeddedMagic << kEmbeddedVersion << static_cast<quint32>(kept.size());
  for (auto it = kept.cbegin(); it != kept.cend(); ++it) {
    const EmbeddedEntry& entry = it.value();
    stream << it.key() << entry.modified << entry.bytes << entry.device << entry.inode << entry.sidecar
           << entry.sidecarModified << entry.sidecarBytes << entry.fields.keywords
           << static_cast<qint8>(entry.fields.rating) << entry.fields.description << entry.fields.hasXmp
           << entry.seen;
  }
  if (stream.status() == QDataStream::Ok && file.size() <= kMaximumEmbeddedCacheBytes && file.commit()) {
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_embedded = std::move(kept);
    m_embeddedDirty = false;
  } else {
    file.cancelWriting();
  }
}

#ifdef OMAROLL_TESTING
void MediaMetadataIndex::insertEmbeddedForTesting(const QString& path, const EmbeddedMetadata::Fields& fields) {
  static qint64 seen = 0;
  EmbeddedEntry entry;
  entry.fields = fields;
  entry.seen = ++seen;
  m_embedded.insert(path, entry);
  m_embeddedDirty = true;
}
#endif

void MediaMetadataIndex::scheduleSave() {
  if (!m_saveTimer.isActive()) {
    m_saveTimer.start();
  }
}

void MediaMetadataIndex::saveCache() {
  // Two files: keyword data never takes room from dates.
  if (m_embeddedDirty) saveEmbeddedCache();
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
