#include "search/OcrIndex.h"

#include "library/CaptureModel.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <utility>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

constexpr quint32 kCacheMagic = 0x4f435231; // OCR1
constexpr quint16 kCacheVersion = 4;
constexpr qint64 kMaximumCacheEntryBytes = 4LL * 1024 * 1024;
constexpr qint64 kMaximumCacheBytes = 64LL * 1024 * 1024;
constexpr int kOcrTimeoutMs = 30'000;
constexpr int kSparseTextThreshold = 6;
constexpr int kDefaultSettleMs = 8'000;
constexpr int kPowerPollMs = 30'000;
// Cached text is restored in batches, so a large warm cache does not hold the
// event loop for one long turn.
constexpr int kCacheHitsPerTurn = 64;

QString readSmall(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(64)).trimmed() : QString();
}

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured;
}

QString layoutText(const QByteArray& bytes) {
  QString text = QString::fromUtf8(bytes);
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  while (text.endsWith(QLatin1Char('\n'))) {
    text.chop(1);
  }
  return text;
}

QString normalizedText(const QString& text) {
  // Search does not need page layout. Collapsing whitespace makes the cache
  // match a phrase across Tesseract's line wrapping. The cache itself keeps
  // the layout-preserving text used by the review sheet.
  return text.simplified();
}

int usefulCharacterCount(const QString& text) {
  int count = 0;
  for (const QChar character : text) {
    if (character.isLetterOrNumber()) {
      ++count;
    }
  }
  return count;
}

} // namespace

OcrIndex::OcrIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model),
      m_program(model ? QStandardPaths::findExecutable(QStringLiteral("tesseract")) : QString()),
      m_languages(qEnvironmentVariable("OMARCHY_OCR_LANGS", QStringLiteral("eng"))),
      m_parallelLimit(std::clamp(QThread::idealThreadCount() / 2, 2, 4)) {
  QStringList languages;
  for (const QString& language : m_languages.split(QLatin1Char('+'), Qt::SkipEmptyParts)) {
    if (!language.trimmed().isEmpty()) languages.append(language.trimmed());
  }
  m_languages = languages.join(QLatin1Char('+'));
  if (m_languages.isEmpty()) {
    m_languages = QStringLiteral("eng");
  }
  // Version 3 did not record languages. Assign those entries once to the
  // configuration present at upgrade, then migrate valid entries on demand.
  QDir().mkpath(cacheDirectory());
  QFile migration(cacheDirectory() + QStringLiteral("/legacy-languages"));
  if (migration.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    migration.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    migration.write(m_languages.toUtf8());
    migration.close();
  }
  if (migration.open(QIODevice::ReadOnly)) {
    m_legacyLanguages = QString::fromUtf8(migration.read(4096));
  }

  m_settleTimer.setSingleShot(true);
  m_settleTimer.setInterval(kDefaultSettleMs);
  connect(&m_settleTimer, &QTimer::timeout, this, [this] {
    if (m_model && m_model->scanning()) {
      return;
    }
    m_settled = true;
    updateBackgroundState();
    if (backgroundRunning()) sync();
  });
  m_powerTimer.setInterval(kPowerPollMs);
  connect(&m_powerTimer, &QTimer::timeout, this, &OcrIndex::checkPower);
  m_coverageTimer.setSingleShot(true);
  m_coverageTimer.setInterval(250);
  connect(&m_coverageTimer, &QTimer::timeout, this, &OcrIndex::updateCoverage);

  if (m_model) {
    connect(m_model, &CaptureModel::countChanged, this, [this] {
      if (m_background && !m_model->scanning()) m_settleTimer.start();
      if (m_active || backgroundRunning()) {
        sync();
      }
    });
    connect(m_model, &CaptureModel::scanningChanged, this, [this] {
      if (m_background && !m_model->scanning()) m_settleTimer.start();
    });
    QThreadPool::globalInstance()->start([] { pruneCache(); });
  }
}

OcrIndex::~OcrIndex() {
  requestStop();
  for (const auto& worker : m_workers) {
    if (worker->process.state() != QProcess::NotRunning) {
      worker->process.kill();
      worker->process.waitForFinished(1000);
    }
  }
}

void OcrIndex::requestStop() {
  m_active = false;
  m_background = false;
  m_queue.clear();
  m_reviewCandidate.reset();
  m_settleTimer.stop();
  m_powerTimer.stop();
  m_coverageTimer.stop();
  for (const auto& worker : m_workers) {
    worker->timeout.stop();
    disconnect(&worker->process, nullptr, this, nullptr);
    if (worker->process.state() != QProcess::NotRunning) worker->process.kill();
  }
}

QString OcrIndex::cacheDirectory() { return cacheHome() + QStringLiteral("/omaroll/ocr"); }

void OcrIndex::setSearchText(const QString& text) {
  const bool active = available() && text.trimmed().size() >= 2;
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (!m_active && !backgroundRunning()) {
    // The picture being read finishes; the search is no longer indexing.
    m_queue.clear();
    setIndexing(false);
    resetProgress(0);
    return;
  }
  sync();
}

void OcrIndex::setBackground(bool enabled) {
  enabled = enabled && available();
  if (m_background == enabled) {
    return;
  }
  m_background = enabled;
  emit backgroundChanged();
  if (enabled) {
    checkPower();
    m_powerTimer.start();
    if (m_model && !m_model->scanning()) m_settleTimer.start();
  } else {
    m_settleTimer.stop();
    m_powerTimer.stop();
    m_settled = false;
  }
  updateBackgroundState();
  if (backgroundRunning() || m_active) {
    sync();
  } else {
    // Off: running processes finish their picture; nothing new starts.
    m_queue.clear();
    setIndexing(false);
    resetProgress(0);
  }
}

void OcrIndex::setScreenshotsOnly(bool value) {
  if (m_screenshotsOnly == value) {
    return;
  }
  m_screenshotsOnly = value;
  emit backgroundChanged();
  if (m_active || backgroundRunning()) sync();
  else scheduleCoverage();
}

QString OcrIndex::pauseReason() const {
  if (!m_background) return {};
  if (m_onBattery) return QStringLiteral("Paused on battery");
  if (!m_settled) return QStringLiteral("Waiting for the library scan to finish");
  return {};
}

bool OcrIndex::backgroundRunning() const { return m_background && m_settled && !m_onBattery; }

int OcrIndex::running() const {
  int count = 0;
  for (const auto& worker : m_workers) {
    if (worker->current) ++count;
  }
  return count;
}

void OcrIndex::setParallelLimit(int limit) {
  m_parallelLimit = std::clamp(limit, 1, 8);
  if (m_active || backgroundRunning()) processNext();
}

void OcrIndex::setSettleDelay(int milliseconds) { m_settleTimer.setInterval(qMax(0, milliseconds)); }

void OcrIndex::setPowerSupplyDirectory(const QString& directory) {
  m_powerDirectory = directory;
  checkPower();
}

bool OcrIndex::onBattery(const QString& directory) {
  const QDir supplies(directory);
  bool battery = false;
  bool discharging = false;
  bool externalKnown = false;
  bool externalOnline = false;
  for (const QString& name : supplies.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    const QString base = supplies.filePath(name) + QLatin1Char('/');
    // Mice, keyboards, controllers and headsets report their own batteries
    // with Device scope; they say nothing about how the computer is powered.
    if (readSmall(base + QStringLiteral("scope")) == QLatin1String("Device")) continue;
    const QString type = readSmall(base + QStringLiteral("type"));
    if (type == QLatin1String("Battery")) {
      battery = true;
      discharging = discharging || readSmall(base + QStringLiteral("status")) ==
                                       QLatin1String("Discharging");
    } else if (type == QLatin1String("Mains") || type.startsWith(QLatin1String("USB"))) {
      const QString online = readSmall(base + QStringLiteral("online"));
      if (!online.isEmpty()) {
        externalKnown = true;
        externalOnline = externalOnline || online == QLatin1String("1");
      }
    }
  }
  // A computer without its own battery runs on mains, whatever its USB-C
  // ports report. One with a battery is on it when nothing external is
  // online and the battery drains or every external supply says offline.
  if (!battery || externalOnline) return false;
  return discharging || externalKnown;
}

void OcrIndex::checkPower() {
  const bool battery = onBattery(m_powerDirectory);
  if (battery == m_onBattery) return;
  m_onBattery = battery;
  updateBackgroundState();
  if (backgroundRunning()) {
    sync();
  } else if (!m_active) {
    m_queue.clear();
    setIndexing(false);
  }
}

void OcrIndex::updateBackgroundState() {
  const bool runningNow = backgroundRunning();
  const QString reason = pauseReason();
  if (runningNow != m_wasRunning || reason != m_lastPauseReason) {
    m_wasRunning = runningNow;
    m_lastPauseReason = reason;
    emit backgroundStateChanged();
  }
}

void OcrIndex::recognize(const QString& path, bool refresh) {
  m_reviewPath = path;
  m_reviewModified = 0;
  m_reviewBytes = 0;
  m_reviewText.clear();
  m_reviewError.clear();
  m_reviewing = true;
  m_reviewCandidate.reset();
  emit reviewChanged();

  if (!available() || !m_model) {
    m_reviewing = false;
    m_reviewError = QStringLiteral("Text recognition is not available");
    emit reviewChanged();
    return;
  }

  const int row = m_model->rowOf(path);
  if (row < 0 || m_model->recordAt(row).isVideo() || m_model->recordAt(row).isDocument() ||
      m_model->recordAt(row).isRaw()) {
    m_reviewing = false;
    m_reviewError = row >= 0 && m_model->recordAt(row).isRaw()
                        ? QStringLiteral("Text recognition cannot read camera raws")
                        : QStringLiteral("This image is no longer available");
    emit reviewChanged();
    return;
  }

  const auto& record = m_model->recordAt(row);
  const Candidate candidate{record.path, record.modified, record.bytes,
                            record.kind == CaptureRecord::Screenshot};
  m_reviewModified = candidate.modified;
  m_reviewBytes = candidate.bytes;
  if (refresh) {
    const bool wasKnown = known(candidate);
    m_entries.remove(candidate.path);
    m_failed.remove(candidate.path);
    QFile::remove(cachePath(candidate.path));
    emit textReady(candidate.path, {});
    noteCoverage(candidate, wasKnown);
  } else {
    const auto entry = m_entries.constFind(candidate.path);
    if (entry != m_entries.cend() && entry->modified == candidate.modified &&
        entry->bytes == candidate.bytes) {
      publishReview(candidate, entry->text);
      return;
    }
    QString cached;
    if (readCache(candidate, &cached)) {
      adopt(candidate, cached);
      publishReview(candidate, cached);
      return;
    }
  }

  if (workerFor(candidate)) {
    return;
  }
  m_reviewCandidate = candidate;
  if (!idleWorker()) {
    // Every worker is busy: free one, preferring a review nobody waits for.
    Worker* target = nullptr;
    for (const auto& worker : m_workers) {
      if (worker->current && worker->forReview && !worker->interrupt) target = worker.get();
    }
    if (!target) {
      for (const auto& worker : m_workers) {
        if (worker->current && !worker->interrupt) target = worker.get();
      }
    }
    if (target) {
      interrupt(target, !target->forReview && queueRunning());
    }
    return;
  }
  processNext();
}

void OcrIndex::cancelReview() {
  Worker* reviewWorker = nullptr;
  for (const auto& worker : m_workers) {
    if (worker->forReview && worker->current && worker->current->path == m_reviewPath) {
      reviewWorker = worker.get();
    }
  }
  m_reviewCandidate.reset();
  if (m_reviewPath.isEmpty() && m_reviewText.isEmpty() && m_reviewError.isEmpty() && !m_reviewing) {
    return;
  }
  m_reviewPath.clear();
  m_reviewModified = 0;
  m_reviewBytes = 0;
  m_reviewText.clear();
  m_reviewError.clear();
  m_reviewing = false;
  emit reviewChanged();
  if (reviewWorker) {
    interrupt(reviewWorker, false);
  }
}

int OcrIndex::clearCache() {
  // Clearing is also an explicit stop. The next edit to an active query can
  // start a fresh index, but this click must not immediately recreate what it
  // just removed. Background indexing resumes on the next library change.
  m_active = false;
  m_settled = false;
  m_queue.clear();
  m_reviewCandidate.reset();
  for (const auto& worker : m_workers) {
    reset(worker.get());
    if (worker->process.state() != QProcess::NotRunning) {
      worker->process.kill();
    }
  }
  emit runningChanged();
  setIndexing(false);
  resetProgress(0);
  updateBackgroundState();

  const int remembered = m_entries.size();
  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
    emit textReady(it.key(), {});
  }
  m_entries.clear();
  m_failed.clear();
  cancelReview();
  scheduleCoverage();

  int removed = 0;
  QDir directory(cacheDirectory());
  for (const QString& fileName : directory.entryList({QStringLiteral("*.ocr")}, QDir::Files)) {
    removed += QFile::remove(directory.filePath(fileName)) ? 1 : 0;
  }
  return qMax(remembered, removed);
}

bool OcrIndex::known(const Candidate& candidate) const {
  const auto entry = m_entries.constFind(candidate.path);
  return (entry != m_entries.cend() && entry->modified == candidate.modified &&
          entry->bytes == candidate.bytes) ||
         failed(candidate);
}

bool OcrIndex::failed(const Candidate& candidate) const {
  const auto found = m_failed.constFind(candidate.path);
  return found != m_failed.cend() && found->modified == candidate.modified &&
         found->bytes == candidate.bytes;
}

void OcrIndex::sync() {
  if ((!m_active && !backgroundRunning()) || !m_model) {
    return;
  }

  QHash<QString, Candidate> live;
  live.reserve(m_model->rowCount());
  QList<Candidate> stills;
  stills.reserve(m_model->rowCount());
  for (int row = 0; row < m_model->rowCount(); ++row) {
    const auto& record = m_model->recordAt(row);
    // Tesseract cannot read a camera raw.
    if (!record.isVideo() && !record.isDocument() && !record.isRaw()) {
      const Candidate candidate{record.path, record.modified, record.bytes,
                                record.kind == CaptureRecord::Screenshot};
      live.insert(record.path, candidate);
      stills.append(candidate);
    }
  }

  for (auto it = m_entries.begin(); it != m_entries.end();) {
    const auto found = live.constFind(it.key());
    if (found == live.cend() || found->modified != it->modified || found->bytes != it->bytes) {
      emit textReady(it.key(), {});
      QFile::remove(cachePath(it.key()));
      it = m_entries.erase(it);
    } else {
      ++it;
    }
  }

  m_queue.clear();
  // Captures are the main search case, so read screenshots before general
  // pictures. Each group remains newest-first in the model's scan order.
  QList<Candidate> screenshots;
  QList<Candidate> pictures;
  for (const Candidate& candidate : std::as_const(stills)) {
    if (known(candidate) || workerFor(candidate)) {
      continue;
    }
    (candidate.screenshot ? screenshots : pictures).append(candidate);
  }
  m_queue = std::move(screenshots);
  if (m_active || !m_screenshotsOnly) {
    m_queue.append(std::move(pictures));
  }
  m_stills = std::move(stills);
  m_stillRows.clear();
  m_stillRows.reserve(m_stills.size());
  for (qsizetype index = 0; index < m_stills.size(); ++index) {
    m_stillRows.insert(m_stills[index].path, int(index));
  }

  int busy = 0;
  for (const auto& worker : m_workers) {
    if (worker->current && !worker->forReview) ++busy;
  }
  resetProgress(m_queue.size() + busy);
  setIndexing(busy > 0 || !m_queue.isEmpty());
  scheduleCoverage();
  processNext();
}

bool OcrIndex::queueRunning() const { return m_active || backgroundRunning(); }

int OcrIndex::workerLimit() const {
  // Without background indexing a search keeps today's single process.
  return m_background ? m_parallelLimit : 1;
}

OcrIndex::Worker* OcrIndex::idleWorker() {
  for (const auto& worker : m_workers) {
    if (!worker->current && worker->process.state() == QProcess::NotRunning) {
      return worker.get();
    }
  }
  if (int(m_workers.size()) >= std::max(workerLimit(), 1)) {
    return nullptr;
  }
  auto worker = std::make_unique<Worker>();
  Worker* raw = worker.get();
  raw->timeout.setSingleShot(true);
  raw->timeout.setInterval(kOcrTimeoutMs);
  connect(&raw->timeout, &QTimer::timeout, &raw->process, &QProcess::kill);
  connect(&raw->process, &QProcess::finished, this,
          [this, raw](int exitCode, QProcess::ExitStatus status) {
            finish(raw, status == QProcess::NormalExit && exitCode == 0);
          });
  connect(&raw->process, &QProcess::errorOccurred, this, [this, raw](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      finish(raw, false);
    }
  });
  m_workers.push_back(std::move(worker));
  return raw;
}

OcrIndex::Worker* OcrIndex::workerFor(const Candidate& candidate) const {
  for (const auto& worker : m_workers) {
    if (worker->current && worker->current->path == candidate.path &&
        worker->current->modified == candidate.modified &&
        worker->current->bytes == candidate.bytes) {
      return worker.get();
    }
  }
  return nullptr;
}

void OcrIndex::processNext() {
  if (m_reviewCandidate) {
    if (Worker* worker = idleWorker()) {
      const Candidate candidate = *m_reviewCandidate;
      m_reviewCandidate.reset();
      if (!stillCurrent(candidate)) {
        if (candidate.path == m_reviewPath) {
          publishReview(candidate, {}, QStringLiteral("This image is no longer available"));
        }
      } else {
        QString cached;
        if (readCache(candidate, &cached)) {
          adopt(candidate, cached);
          if (candidate.path == m_reviewPath) publishReview(candidate, cached);
        } else {
          start(worker, candidate, true);
        }
      }
    }
  }

  int cacheHits = 0;
  while (queueRunning() && !m_queue.isEmpty()) {
    int busy = 0;
    for (const auto& worker : m_workers) {
      if (worker->current) ++busy;
    }
    if (busy >= workerLimit()) break;
    // Background work alone, set to screenshots, stops at the first picture.
    if (!m_active && m_screenshotsOnly && !m_queue.first().screenshot) {
      m_queue.clear();
      break;
    }
    const Candidate candidate = m_queue.takeFirst();
    if (!stillCurrent(candidate) || failed(candidate)) {
      advanceProgress();
      continue;
    }
    QString cached;
    if (readCache(candidate, &cached)) {
      adopt(candidate, cached);
      advanceProgress();
      if (++cacheHits >= kCacheHitsPerTurn) {
        QTimer::singleShot(0, this, &OcrIndex::processNext);
        return;
      }
      continue;
    }
    Worker* worker = idleWorker();
    if (!worker) {
      m_queue.prepend(candidate);
      break;
    }
    start(worker, candidate, false);
  }

  if (running() == 0 && !m_reviewCandidate && (m_queue.isEmpty() || !queueRunning())) {
    setIndexing(false);
  }
}

void OcrIndex::start(Worker* worker, const Candidate& candidate, bool forReview) {
  worker->current = candidate;
  worker->forReview = forReview;
  worker->sparseTextPass = false;
  worker->interrupt = false;
  worker->requeue = false;
  worker->firstPassText.clear();
  if (!forReview) setIndexing(true);
  emit runningChanged();
  startProcess(worker, false);
}

void OcrIndex::startProcess(Worker* worker, bool sparseText) {
  if (!worker->current) {
    return;
  }
  worker->sparseTextPass = sparseText;
  QProcess& process = worker->process;
  process.setProgram(m_program);
  QStringList arguments = {worker->current->path, QStringLiteral("stdout"),
                           QStringLiteral("--oem"), QStringLiteral("1"),
                           QStringLiteral("-l"),    m_languages,
                           QStringLiteral("--dpi"), QStringLiteral("300")};
  if (sparseText) {
    arguments.append({QStringLiteral("--psm"), QStringLiteral("11")});
  }
  arguments.append({QStringLiteral("-c"), QStringLiteral("preserve_interword_spaces=1")});
  process.setArguments(arguments);
  process.setProcessChannelMode(QProcess::SeparateChannels);
  // Reading in the background runs at the lowest CPU and I/O priority, one
  // thread per process; a review someone waits for does not.
  const bool lowPriority = m_background && !worker->forReview;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  if (lowPriority) environment.insert(QStringLiteral("OMP_THREAD_LIMIT"), QStringLiteral("1"));
  process.setProcessEnvironment(environment);
  process.setChildProcessModifier([lowPriority] {
    if (lowPriority) {
      (void)::setpriority(PRIO_PROCESS, 0, 19);
      // IOPRIO_WHO_PROCESS, this process, IOPRIO_CLASS_IDLE.
      (void)::syscall(SYS_ioprio_set, 1, 0, 3 << 13);
    }
  });
  process.start(QIODevice::ReadOnly);
  worker->timeout.start();
}

void OcrIndex::interrupt(Worker* worker, bool requeue) {
  if (!worker->current || worker->process.state() == QProcess::NotRunning) {
    return;
  }
  worker->interrupt = true;
  worker->requeue = requeue;
  worker->timeout.stop();
  worker->process.kill();
}

void OcrIndex::reset(Worker* worker) {
  worker->timeout.stop();
  worker->current.reset();
  worker->forReview = false;
  worker->sparseTextPass = false;
  worker->interrupt = false;
  worker->requeue = false;
  worker->firstPassText.clear();
}

void OcrIndex::finish(Worker* worker, bool successful) {
  if (!worker->current) {
    if (queueRunning() || m_reviewCandidate) {
      QTimer::singleShot(0, this, &OcrIndex::processNext);
    }
    return;
  }
  worker->timeout.stop();
  const Candidate candidate = *worker->current;
  const bool forReview = worker->forReview;
  const bool sparseTextPass = worker->sparseTextPass;
  if (worker->interrupt) {
    const bool requeue = worker->requeue;
    reset(worker);
    emit runningChanged();
    worker->process.readAllStandardOutput();
    worker->process.readAllStandardError();
    if (requeue && queueRunning() && stillCurrent(candidate)) {
      m_queue.prepend(candidate);
    }
    QTimer::singleShot(0, this, &OcrIndex::processNext);
    return;
  }
  const QString passText =
      successful ? layoutText(worker->process.readAllStandardOutput()) : QString();

  const bool reviewStillWanted = forReview && candidate.path == m_reviewPath &&
                                 candidate.modified == m_reviewModified &&
                                 candidate.bytes == m_reviewBytes;
  if (!sparseTextPass && successful && stillCurrent(candidate) && reviewStillWanted &&
      !m_reviewCandidate && usefulCharacterCount(passText) < kSparseTextThreshold) {
    worker->firstPassText = passText;
    QTimer::singleShot(0, this, [this, worker] {
      if (worker->current) {
        startProcess(worker, true);
      }
    });
    return;
  }

  const QString firstPassText = worker->firstPassText;
  reset(worker);
  emit runningChanged();

  const bool hasSuccessfulFirstPass = sparseTextPass && stillCurrent(candidate);
  if ((successful || hasSuccessfulFirstPass) && stillCurrent(candidate)) {
    const QString text =
        sparseTextPass && usefulCharacterCount(firstPassText) >= usefulCharacterCount(passText)
            ? firstPassText
            : passText;
    writeCache(candidate, text);
    adopt(candidate, text);
    if (candidate.path == m_reviewPath) {
      publishReview(candidate, text);
    }
  } else {
    const bool wasKnown = known(candidate);
    m_failed.insert(candidate.path, {candidate.modified, candidate.bytes});
    noteCoverage(candidate, wasKnown);
    if (candidate.path == m_reviewPath) {
      publishReview(candidate, {}, QStringLiteral("Could not extract text"));
    }
  }
  if (!forReview) {
    advanceProgress();
  }

  if (!queueRunning() && !m_reviewCandidate) {
    if (running() == 0) setIndexing(false);
    return;
  }
  QTimer::singleShot(0, this, &OcrIndex::processNext);
}

void OcrIndex::setIndexing(bool value) {
  if (m_indexing == value) {
    return;
  }
  m_indexing = value;
  emit indexingChanged();
}

void OcrIndex::resetProgress(int total) {
  total = qMax(0, total);
  if (m_completed == 0 && m_total == total) {
    return;
  }
  m_completed = 0;
  m_total = total;
  emit progressChanged();
}

void OcrIndex::advanceProgress() {
  if (m_completed >= m_total) {
    return;
  }
  ++m_completed;
  emit progressChanged();
}

void OcrIndex::scheduleCoverage() {
  m_coverageRecount = true;
  if (!m_coverageTimer.isActive()) m_coverageTimer.start();
}

// One still read or forgotten: adjust the counts rather than recount them,
// which on a large library would be a pass over every still per result.
void OcrIndex::noteCoverage(const Candidate& candidate, bool wasKnown) {
  const auto row = m_stillRows.constFind(candidate.path);
  if (row == m_stillRows.cend()) return;
  const Candidate& still = m_stills.at(row.value());
  if (still.modified != candidate.modified || still.bytes != candidate.bytes) return;
  const bool nowKnown = known(still);
  if (nowKnown == wasKnown) return;
  const int step = nowKnown ? 1 : -1;
  m_unreadCount -= step;
  if (!m_screenshotsOnly || still.screenshot) m_indexedCount += step;
  // Signal at most a few times a second.
  m_coverageSignal = true;
  if (!m_coverageTimer.isActive()) m_coverageTimer.start();
}

void OcrIndex::updateCoverage() {
  if (!m_coverageRecount) {
    if (m_coverageSignal) {
      m_coverageSignal = false;
      emit coverageChanged();
    }
    return;
  }
  m_coverageRecount = false;
  const bool pending = std::exchange(m_coverageSignal, false);
  int indexed = 0;
  int eligible = 0;
  int unread = 0;
  for (const Candidate& candidate : std::as_const(m_stills)) {
    const bool read = known(candidate);
    if (!read) ++unread;
    if (!m_screenshotsOnly || candidate.screenshot) {
      ++eligible;
      if (read) ++indexed;
    }
  }
  if (pending || indexed != m_indexedCount || eligible != m_eligibleCount ||
      unread != m_unreadCount) {
    m_indexedCount = indexed;
    m_eligibleCount = eligible;
    m_unreadCount = unread;
    emit coverageChanged();
  }
}

bool OcrIndex::stillCurrent(const Candidate& candidate) const {
  if (!m_model) {
    return false;
  }
  const int row = m_model->rowOf(candidate.path);
  if (row < 0) {
    return false;
  }
  const auto& record = m_model->recordAt(row);
  return !record.isVideo() && !record.isDocument() && record.modified == candidate.modified &&
         record.bytes == candidate.bytes;
}

QString OcrIndex::cachePath(const QString& path) const {
  const QByteArray key =
      QCryptographicHash::hash(path.toUtf8() + '\0' + m_languages.toUtf8(),
                               QCryptographicHash::Sha256).toHex();
  return cacheDirectory() + QLatin1Char('/') + QString::fromLatin1(key) + QStringLiteral(".ocr");
}

bool OcrIndex::readCache(const Candidate& candidate, QString* text) const {
  QFile file(cachePath(candidate.path));
  bool legacy = false;
  if (!file.open(QIODevice::ReadOnly) && m_languages == m_legacyLanguages) {
    const QByteArray key = QCryptographicHash::hash(candidate.path.toUtf8(),
                                                   QCryptographicHash::Sha256).toHex();
    file.setFileName(cacheDirectory() + QLatin1Char('/') + QString::fromLatin1(key) +
                     QStringLiteral(".ocr"));
    legacy = file.open(QIODevice::ReadOnly);
  }
  if (!file.isOpen() || file.size() > kMaximumCacheEntryBytes) {
    return false;
  }
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  quint32 magic = 0;
  quint16 version = 0;
  qint64 modified = 0;
  qint64 bytes = 0;
  QString path;
  QString languages;
  QString stored;
  stream >> magic >> version >> path >> modified >> bytes;
  if (version == 4) stream >> languages;
  stream >> stored;
  if (stream.status() != QDataStream::Ok || magic != kCacheMagic ||
      (legacy ? version != 3 : version != kCacheVersion) ||
      path != candidate.path || (!legacy && languages != m_languages) ||
      modified != candidate.modified || bytes != candidate.bytes ||
      stored.size() > kMaximumCacheEntryBytes) {
    return false;
  }
  *text = stored;
  if (legacy) {
    writeCache(candidate, stored);
    if (QFileInfo::exists(cachePath(candidate.path))) QFile::remove(file.fileName());
  }
  return true;
}

void OcrIndex::writeCache(const Candidate& candidate, const QString& text) const {
  const QString directory = cacheDirectory();
  if (!QDir().mkpath(directory)) {
    return;
  }
  QFile::setPermissions(directory,
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

  QSaveFile file(cachePath(candidate.path));
  if (!file.open(QIODevice::WriteOnly)) {
    return;
  }
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << kCacheMagic << kCacheVersion << candidate.path << candidate.modified << candidate.bytes
         << m_languages << text;
  if (stream.status() == QDataStream::Ok && file.commit()) {
    QFile::setPermissions(cachePath(candidate.path),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
}

void OcrIndex::pruneCache() {
  QDir directory(cacheDirectory());
  const QFileInfoList files =
      directory.entryInfoList({QStringLiteral("*.ocr")}, QDir::Files, QDir::Time);
  qint64 retainedBytes = 0;
  for (const QFileInfo& info : files) {
    bool keep = info.size() <= kMaximumCacheEntryBytes;
    QFile file(info.filePath());
    if (keep && file.open(QIODevice::ReadOnly)) {
      QDataStream stream(&file);
      stream.setVersion(QDataStream::Qt_6_0);
      quint32 magic = 0;
      quint16 version = 0;
      QString path;
      qint64 modified = 0;
      qint64 bytes = 0;
      QString languages;
      QString text;
      stream >> magic >> version >> path >> modified >> bytes;
      if (version == 4) stream >> languages;
      stream >> text;
      const QFileInfo source(path);
      keep = stream.status() == QDataStream::Ok && magic == kCacheMagic &&
             (version == 3 || version == kCacheVersion) && !path.isEmpty() && source.isFile() &&
             source.size() == bytes && source.lastModified().toMSecsSinceEpoch() == modified &&
             retainedBytes + info.size() <= kMaximumCacheBytes;
    } else {
      keep = false;
    }
    if (keep) {
      retainedBytes += info.size();
    } else {
      QFile::remove(info.filePath());
    }
  }
}

void OcrIndex::adopt(const Candidate& candidate, const QString& text) {
  const bool wasKnown = known(candidate);
  m_entries.insert(candidate.path, {candidate.modified, candidate.bytes, text});
  emit textReady(candidate.path, normalizedText(text));
  noteCoverage(candidate, wasKnown);
}

void OcrIndex::publishReview(const Candidate& candidate, const QString& text,
                             const QString& error) {
  if (candidate.path != m_reviewPath || candidate.modified != m_reviewModified ||
      candidate.bytes != m_reviewBytes) {
    return;
  }
  m_reviewText = text;
  m_reviewError = error;
  m_reviewing = false;
  emit reviewChanged();
}
