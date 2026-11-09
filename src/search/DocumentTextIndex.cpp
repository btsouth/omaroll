#include "search/DocumentTextIndex.h"

#include "library/CaptureModel.h"
#include "pdf/PdfSupport.h"
#include "search/OcrIndex.h"
#include "sources/FileVersion.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {

constexpr quint32 kCacheMagic = 0x50445431; // PDT1
// 2: the text as compressed UTF-8. 3: keyed by size, mtime and ctime rather
// than size and mtime alone. Older entries are pruned.
constexpr quint16 kCacheVersion = 3;
constexpr qint64 kMaximumCacheBytes = 64LL * 1024 * 1024;
constexpr int kPowerPollMs = 30'000;

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured;
}

struct CacheEntry {
  bool valid = false;
  QString path;
  QString key;
  QByteArray compressed;
};

// The header and, when |withText|, the compressed text of one cache file.
CacheEntry readEntry(const QString& file, qint64 limit, bool withText) {
  CacheEntry entry;
  QFile input(file);
  if (!input.open(QIODevice::ReadOnly) || input.size() > limit) return entry;
  QDataStream stream(&input);
  stream.setVersion(QDataStream::Qt_6_0);
  quint32 magic = 0;
  quint16 version = 0;
  stream >> magic >> version;
  if (stream.status() != QDataStream::Ok || magic != kCacheMagic || version != kCacheVersion) {
    return entry;
  }
  stream >> entry.path >> entry.key;
  if (withText) stream >> entry.compressed;
  entry.valid = stream.status() == QDataStream::Ok;
  return entry;
}

// The text of a valid entry, or a null string when it does not decompress.
QString entryText(const CacheEntry& entry, qsizetype limit) {
  if (entry.compressed.isEmpty()) return QString(QLatin1String(""));
  const QByteArray text = qUncompress(entry.compressed);
  if (text.isEmpty()) return {};
  return QString::fromUtf8(text).left(limit);
}

} // namespace

DocumentTextIndex::DocumentTextIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model) {
  m_syncTimer.setSingleShot(true);
  m_syncTimer.setInterval(400);
  connect(&m_syncTimer, &QTimer::timeout, this, &DocumentTextIndex::sync);
  connect(&m_watcher, &QFutureWatcher<Result>::finished, this, &DocumentTextIndex::finished);
  m_powerTimer.setInterval(kPowerPollMs);
  connect(&m_powerTimer, &QTimer::timeout, this, &DocumentTextIndex::checkPower);
  if (m_model) {
    connect(m_model, &CaptureModel::countChanged, this, [this] {
      if (wanted()) m_syncTimer.start();
    });
    connect(m_model, &CaptureModel::scanningChanged, this, [this] {
      if (wanted()) m_syncTimer.start();
    });
    QThreadPool::globalInstance()->start([] { pruneCache(); });
  }
}

DocumentTextIndex::~DocumentTextIndex() {
  requestStop();
  // The worker holds no pointer to this object; waiting only keeps a cache
  // write from outliving the process's teardown.
  m_watcher.waitForFinished();
}

void DocumentTextIndex::requestStop() {
  m_stopped = true;
  m_cancel->store(true);
  m_queue.clear();
  m_syncTimer.stop();
  m_powerTimer.stop();
}

QString DocumentTextIndex::cacheDirectory() {
  return cacheHome() + QStringLiteral("/omaroll/pdftext");
}

QString DocumentTextIndex::cachePath(const QString& path) {
  const QByteArray key = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
  return cacheDirectory() + QLatin1Char('/') + QString::fromLatin1(key) + QStringLiteral(".txt");
}

bool DocumentTextIndex::wanted() const {
  // A search reads on any power; background indexing waits for mains.
  return !m_stopped && m_model &&
         (m_searching || (m_background && !m_onBattery && !m_model->scanning()));
}

void DocumentTextIndex::setPowerSupplyDirectory(const QString& directory) {
  m_powerDirectory = directory;
  checkPower();
}

void DocumentTextIndex::checkPower() {
  const bool battery = OcrIndex::onBattery(m_powerDirectory);
  if (battery == m_onBattery) return;
  m_onBattery = battery;
  if (wanted()) sync();
  else m_queue.clear();
}

void DocumentTextIndex::setSearchText(const QString& text) {
  const bool searching = text.trimmed().size() >= 2;
  if (searching == m_searching) return;
  m_searching = searching;
  if (wanted()) {
    sync();
  } else {
    // The document being read finishes; nothing new starts.
    m_queue.clear();
  }
}

void DocumentTextIndex::setBackground(bool enabled) {
  if (enabled == m_background) return;
  m_background = enabled;
  if (enabled && !m_stopped) {
    checkPower();
    m_powerTimer.start();
  } else {
    m_powerTimer.stop();
  }
  if (wanted()) sync();
  else m_queue.clear();
}

bool DocumentTextIndex::known(const Candidate& candidate) const {
  const auto same = [&candidate](const QHash<QString, Candidate>& map) {
    const auto found = map.constFind(candidate.path);
    return found != map.cend() && found->sameVersion(candidate);
  };
  return same(m_known) || same(m_failed);
}

bool DocumentTextIndex::current(const Candidate& candidate) const {
  if (!m_model) return false;
  const int row = m_model->rowOf(candidate.path);
  if (row < 0) return false;
  const CaptureRecord& record = m_model->recordAt(row);
  return record.isDocument() && record.modified == candidate.modified &&
         record.bytes == candidate.bytes && record.thumbnailVersion == candidate.version;
}

void DocumentTextIndex::sync() {
  if (!m_model) return;
  QHash<QString, Candidate> live;
  QList<Candidate> documents;
  for (int row = 0; row < m_model->rowCount(); ++row) {
    const CaptureRecord& record = m_model->recordAt(row);
    if (!record.isDocument()) continue;
    const Candidate candidate{record.path, record.modified, record.bytes,
                              record.thumbnailVersion};
    live.insert(record.path, candidate);
    documents.append(candidate);
  }
  // Forget documents that left the library or changed since they were read.
  for (auto it = m_known.begin(); it != m_known.end();) {
    const auto found = live.constFind(it.key());
    if (found == live.cend() || !found->sameVersion(*it)) {
      emit textReady(it.key(), {}, false);
      it = m_known.erase(it);
    } else {
      ++it;
    }
  }
  // A failed document that changed gets another try.
  m_failed.removeIf([&live](const QHash<QString, Candidate>::iterator it) {
    const auto found = live.constFind(it.key());
    return found == live.cend() || !found->sameVersion(it.value());
  });
  if (!wanted()) return;
  // The model holds the newest scan results first.
  m_queue.clear();
  for (const Candidate& candidate : std::as_const(documents)) {
    if (!known(candidate)) m_queue.append(candidate);
  }
  processNext();
}

void DocumentTextIndex::processNext() {
  if (m_running) return;
  while (!m_queue.isEmpty() && wanted()) {
    const Candidate candidate = m_queue.takeFirst();
    if (known(candidate) || !current(candidate)) continue;
    m_running = true;
    m_current = candidate;
    setIndexing(true);
    const QString program = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    const QString cache = cachePath(candidate.path);
    m_watcher.setFuture(QtConcurrent::run(
        [candidate, program, cache, timeout = m_timeoutMs, cancel = m_cancel,
         generation = m_generation] {
          Result result = extract(candidate, program, cache, timeout, *cancel);
          result.generation = generation;
          return result;
        }));
    return;
  }
  setIndexing(false);
}

void DocumentTextIndex::finished() {
  m_running = false;
  m_current = {};
  const Result result = m_watcher.result();
  if (result.generation != m_generation) {
    // Read before the cache was cleared: neither kept nor shown.
    QFile::remove(cachePath(result.candidate.path));
  } else if (result.read && current(result.candidate)) {
    m_known.insert(result.candidate.path, result.candidate);
    if (!result.text.isEmpty()) {
      emit textReady(result.candidate.path, result.text, result.persisted);
    }
  } else if (!result.read && !m_stopped && current(result.candidate)) {
    m_failed.insert(result.candidate.path, result.candidate);
  }
  if (m_stopped) {
    setIndexing(false);
    return;
  }
  processNext();
}

DocumentTextIndex::Result DocumentTextIndex::extract(const Candidate& candidate,
                                                     const QString& program,
                                                     const QString& cachePath,
                                                     int timeoutMs,
                                                     const std::atomic_bool& cancel) {
  Result result{candidate, {}, false};
  // Taken before reading, so a change while the tool runs is caught next
  // time rather than cached as current.
  const QString key = FileVersion::diskKey(candidate.path);
  if (key.isEmpty()) return result;
  {
    const CacheEntry entry = readEntry(cachePath, kTextLimit * 4, true);
    if (entry.valid && entry.path == candidate.path && entry.key == key) {
      const QString text = entryText(entry, kTextLimit);
      if (!text.isNull()) {
        result.text = text;
        result.read = true;
        result.persisted = true;
        return result;
      }
    }
  }
  if (program.isEmpty()) return result;

  QProcess process;
  PdfSupport::limitProcess(process);
  process.setStandardErrorFile(QProcess::nullDevice());
  process.start(program, {QStringLiteral("-q"), QStringLiteral("-enc"), QStringLiteral("UTF-8"),
                          QStringLiteral("-l"), QString::number(kPageLimit), candidate.path,
                          QStringLiteral("-")});
  if (!process.waitForStarted(5000)) return result;
  // Text beyond the limit is not searched, so the tool is stopped there
  // rather than buffering a whole book.
  QByteArray output;
  const qsizetype byteLimit = kTextLimit * 4;
  QDeadlineTimer deadline(timeoutMs);
  bool truncated = false;
  while (process.state() != QProcess::NotRunning) {
    if (deadline.hasExpired() || cancel.load()) {
      process.kill();
      process.waitForFinished(1000);
      return result;
    }
    process.waitForReadyRead(100);
    output += process.readAllStandardOutput();
    if (output.size() >= byteLimit) {
      truncated = true;
      process.kill();
      process.waitForFinished(1000);
      break;
    }
  }
  output += process.readAllStandardOutput();
  if (!truncated &&
      (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)) {
    // Encrypted or broken documents are remembered as having no text, so
    // they are not tried again until they change.
    output.clear();
  }
  result.text = QString::fromUtf8(output.left(byteLimit)).simplified().left(kTextLimit);
  result.read = true;

  if (QDir().mkpath(cacheDirectory())) {
    QFile::setPermissions(cacheDirectory(),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile file(cachePath);
    if (file.open(QIODevice::WriteOnly)) {
      QDataStream stream(&file);
      stream.setVersion(QDataStream::Qt_6_0);
      const QByteArray text = result.text.toUtf8();
      stream << kCacheMagic << kCacheVersion << candidate.path << key
             << (text.isEmpty() ? QByteArray() : qCompress(text));
      if (stream.status() == QDataStream::Ok && file.commit()) {
        QFile::setPermissions(cachePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        result.persisted = true;
      }
    }
  }
  return result;
}

QString DocumentTextIndex::cachedText(const QString& path) {
  const CacheEntry entry = readEntry(cachePath(path), kTextLimit * 4, true);
  if (!entry.valid || entry.path != path || entry.key != FileVersion::diskKey(path)) return {};
  return entryText(entry, kTextLimit);
}

void DocumentTextIndex::reread(const QString& path) {
  if (m_known.remove(path) == 0) return;
  if (wanted()) m_syncTimer.start();
}

int DocumentTextIndex::clearCache() {
  // A sync already scheduled would start reading again into the new cache.
  m_syncTimer.stop();
  m_queue.clear();
  ++m_generation;
  const int remembered = m_known.size();
  for (auto it = m_known.cbegin(); it != m_known.cend(); ++it) emit textReady(it.key(), {}, false);
  if (m_running && !m_known.contains(m_current.path)) emit textReady(m_current.path, {}, false);
  m_known.clear();
  m_failed.clear();
  int removed = 0;
  QDir directory(cacheDirectory());
  for (const QString& name : directory.entryList({QStringLiteral("*.txt")}, QDir::Files)) {
    removed += QFile::remove(directory.filePath(name)) ? 1 : 0;
  }
  return std::max(remembered, removed);
}

void DocumentTextIndex::pruneCache() {
  QDir directory(cacheDirectory());
  const QFileInfoList files =
      directory.entryInfoList({QStringLiteral("*.txt")}, QDir::Files, QDir::Time);
  qint64 retained = 0;
  for (const QFileInfo& info : files) {
    const CacheEntry entry = readEntry(info.filePath(), kTextLimit * 4, false);
    const bool keep = entry.valid && !entry.path.isEmpty() &&
                      entry.key == FileVersion::diskKey(entry.path) &&
                      retained + info.size() <= kMaximumCacheBytes;
    if (keep) retained += info.size();
    else QFile::remove(info.filePath());
  }
}

void DocumentTextIndex::setIndexing(bool value) {
  if (m_indexing == value) return;
  m_indexing = value;
  emit indexingChanged();
}
