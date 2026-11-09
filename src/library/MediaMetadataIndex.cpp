#include "library/MediaMetadataIndex.h"
#include "app/StartupTrace.h"

#include "sources/CameraRaw.h"

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

} // namespace

struct MediaMetadataIndex::Probe {
  enum class Kind { Image, Video, Raw };
  QList<Candidate> batch;
  Kind kind = Kind::Image;
  // Removed from the library while running: the result is discarded and the
  // process is stopped early.
  bool cancelled = false;
  // The same, for a raw probe's worker, which stops between files.
  std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);
  bool parsing = false;
  bool finished = false;
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
  if (m_cacheDirty) {
    saveCache();
  }
}

void MediaMetadataIndex::requestStop() {
  m_stopping->store(true);
  m_syncTimer.stop();
  m_updateTimer.stop();
  m_saveTimer.stop();
  m_progressTimer.stop();
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

void MediaMetadataIndex::sync() {
  if (m_stopping->load() || !m_model) {
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
          (videos ? m_videoProgram.isEmpty() : m_imageProgram.isEmpty() && !record.isRaw())) {
        continue;
      }

      const Candidate candidate {record.path,      record.modified, record.bytes,
                                 record.isVideo(), record.device,   record.inode,
                                 record.isRaw()};
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

  for (const CaptureModel::MetadataUpdate& update : std::as_const(cachedUpdates)) {
    m_pendingUpdates.insert(update.path, update);
  }
  if (!m_pendingUpdates.isEmpty() && !m_updateTimer.isActive()) m_updateTimer.start();
  cancelStaleProbes();
  resetProgress(static_cast<int>(m_queue.size()) + inFlightCount());
  setIndexing(inFlightCount() > 0 || !m_queue.isEmpty());
  if (m_cacheDirty) {
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
  probe->kind = first.raw ? Probe::Kind::Raw : first.video ? Probe::Kind::Video : Probe::Kind::Image;
  if (probe->kind != Probe::Kind::Video) {
    // Raws are a few header reads each; images share one ImageMagick run.
    while (batch.size() < kImageBatchSize && !m_queue.isEmpty() && !m_queue.first().video &&
           m_queue.first().raw == first.raw) {
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

  if (raw->kind == Probe::Kind::Raw) {
    QStringList paths;
    for (const Candidate& candidate : std::as_const(raw->batch)) paths.append(candidate.path);
    const auto stopping = m_stopping;
    const auto stop = raw->stop;
    raw->parse.setFuture(QtConcurrent::run(&m_pool, [paths, stopping, stop] {
      lowerThisThread();
      Parsed parsed;
      for (int index = 0; index < paths.size(); ++index) {
        if (stopping->load() || stop->load()) break;
        parsed.results.insert(index, rawDetails(paths.at(index)));
        parsed.completed.insert(index);
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
  // Only the copy of the output crosses to the worker.
  probe->parse.setFuture(QtConcurrent::run(&m_pool, [output, video, successful] {
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
      if (parsed.completed.contains(index)) {
        adopt(candidate, parsed.results.value(index));
      } else if (probe->kind != Probe::Kind::Raw) {
        m_failed.insert(candidate.path, identityKey(candidate.modified, candidate.bytes,
                                                    candidate.device, candidate.inode));
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
      if (current.path == candidate.path && current.modified == candidate.modified &&
          current.bytes == candidate.bytes && current.device == candidate.device &&
          current.inode == candidate.inode && current.video == candidate.video) {
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
