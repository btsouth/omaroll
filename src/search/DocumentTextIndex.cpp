#include "search/DocumentTextIndex.h"

#include "library/CaptureModel.h"
#include "pdf/PdfSupport.h"

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
constexpr quint16 kCacheVersion = 1;
constexpr qint64 kMaximumCacheBytes = 64LL * 1024 * 1024;
constexpr int kExtractTimeoutMs = 20'000;

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured;
}

} // namespace

DocumentTextIndex::DocumentTextIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model) {
  m_syncTimer.setSingleShot(true);
  m_syncTimer.setInterval(400);
  connect(&m_syncTimer, &QTimer::timeout, this, &DocumentTextIndex::sync);
  connect(&m_watcher, &QFutureWatcher<Result>::finished, this, &DocumentTextIndex::finished);
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
}

QString DocumentTextIndex::cacheDirectory() {
  return cacheHome() + QStringLiteral("/omaroll/pdftext");
}

QString DocumentTextIndex::cachePath(const QString& path) {
  const QByteArray key = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
  return cacheDirectory() + QLatin1Char('/') + QString::fromLatin1(key) + QStringLiteral(".txt");
}

bool DocumentTextIndex::wanted() const {
  return !m_stopped && m_model && (m_searching || (m_background && !m_model->scanning()));
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
  if (wanted()) sync();
  else m_queue.clear();
}

bool DocumentTextIndex::known(const Candidate& candidate) const {
  const auto found = m_known.constFind(candidate.path);
  return found != m_known.cend() && found->modified == candidate.modified &&
         found->bytes == candidate.bytes;
}

bool DocumentTextIndex::current(const Candidate& candidate) const {
  if (!m_model) return false;
  const int row = m_model->rowOf(candidate.path);
  if (row < 0) return false;
  const CaptureRecord& record = m_model->recordAt(row);
  return record.isDocument() && record.modified == candidate.modified &&
         record.bytes == candidate.bytes;
}

void DocumentTextIndex::sync() {
  if (!m_model) return;
  QHash<QString, Candidate> live;
  QList<Candidate> documents;
  for (int row = 0; row < m_model->rowCount(); ++row) {
    const CaptureRecord& record = m_model->recordAt(row);
    if (!record.isDocument()) continue;
    const Candidate candidate{record.path, record.modified, record.bytes};
    live.insert(record.path, candidate);
    documents.append(candidate);
  }
  // Forget documents that left the library or changed since they were read.
  for (auto it = m_known.begin(); it != m_known.end();) {
    const auto found = live.constFind(it.key());
    if (found == live.cend() || found->modified != it->modified || found->bytes != it->bytes) {
      emit textReady(it.key(), {});
      it = m_known.erase(it);
    } else {
      ++it;
    }
  }
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
        [candidate, program, cache, cancel = m_cancel, generation = m_generation] {
          Result result = extract(candidate, program, cache, *cancel);
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
    if (!result.text.isEmpty()) emit textReady(result.candidate.path, result.text);
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
                                                     const std::atomic_bool& cancel) {
  Result result{candidate, {}, false};
  {
    QFile file(cachePath);
    if (file.open(QIODevice::ReadOnly) && file.size() <= kTextLimit * 4) {
      QDataStream stream(&file);
      stream.setVersion(QDataStream::Qt_6_0);
      quint32 magic = 0;
      quint16 version = 0;
      QString path;
      qint64 modified = 0;
      qint64 bytes = 0;
      QString text;
      stream >> magic >> version >> path >> modified >> bytes >> text;
      if (stream.status() == QDataStream::Ok && magic == kCacheMagic &&
          version == kCacheVersion && path == candidate.path &&
          modified == candidate.modified && bytes == candidate.bytes) {
        result.text = text.left(kTextLimit);
        result.read = true;
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
  QDeadlineTimer deadline(kExtractTimeoutMs);
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
      stream << kCacheMagic << kCacheVersion << candidate.path << candidate.modified
             << candidate.bytes << result.text;
      if (stream.status() == QDataStream::Ok && file.commit()) {
        QFile::setPermissions(cachePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      }
    }
  }
  return result;
}

int DocumentTextIndex::clearCache() {
  m_queue.clear();
  ++m_generation;
  const int remembered = m_known.size();
  for (auto it = m_known.cbegin(); it != m_known.cend(); ++it) emit textReady(it.key(), {});
  if (m_running && !m_known.contains(m_current.path)) emit textReady(m_current.path, {});
  m_known.clear();
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
    bool keep = false;
    QFile file(info.filePath());
    if (info.size() <= kTextLimit * 4 && file.open(QIODevice::ReadOnly)) {
      QDataStream stream(&file);
      stream.setVersion(QDataStream::Qt_6_0);
      quint32 magic = 0;
      quint16 version = 0;
      QString path;
      qint64 modified = 0;
      qint64 bytes = 0;
      stream >> magic >> version >> path >> modified >> bytes;
      const QFileInfo source(path);
      keep = stream.status() == QDataStream::Ok && magic == kCacheMagic &&
             version == kCacheVersion && source.isFile() && source.size() == bytes &&
             source.lastModified().toMSecsSinceEpoch() == modified &&
             retained + info.size() <= kMaximumCacheBytes;
    }
    if (keep) retained += info.size();
    else QFile::remove(info.filePath());
  }
}

void DocumentTextIndex::setIndexing(bool value) {
  if (m_indexing == value) return;
  m_indexing = value;
  emit indexingChanged();
}
