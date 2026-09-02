#include "search/OcrIndex.h"

#include "library/CaptureModel.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>

namespace {

constexpr quint32 kCacheMagic = 0x4f435231; // OCR1
constexpr quint16 kCacheVersion = 3;
constexpr qint64 kMaximumCacheEntryBytes = 4 * 1024 * 1024;
constexpr qint64 kMaximumCacheBytes = 64 * 1024 * 1024;
constexpr int kOcrTimeoutMs = 30'000;

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache")
                              : configured;
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

} // namespace

OcrIndex::OcrIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model),
      m_program(model ? QStandardPaths::findExecutable(QStringLiteral("tesseract"))
                      : QString()),
      m_languages(qEnvironmentVariable("OMARCHY_OCR_LANGS", QStringLiteral("eng"))) {
  if (m_languages.isEmpty()) {
    m_languages = QStringLiteral("eng");
  }
  m_timeout.setSingleShot(true);
  m_timeout.setInterval(kOcrTimeoutMs);

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

  if (m_model) {
    connect(m_model, &CaptureModel::countChanged, this, [this] {
      if (m_active) {
        sync();
      }
    });
    QThreadPool::globalInstance()->start([] { pruneCache(); });
  }
}

OcrIndex::~OcrIndex() {
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
}

QString OcrIndex::cacheDirectory() {
  return cacheHome() + QStringLiteral("/omaroll/ocr");
}

void OcrIndex::setSearchText(const QString& text) {
  const bool active = available() && text.trimmed().size() >= 2;
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (!m_active) {
    m_queue.clear();
    setIndexing(false);
    resetProgress(0);
    return;
  }
  sync();
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
  if (row < 0 || m_model->recordAt(row).isVideo()) {
    m_reviewing = false;
    m_reviewError = QStringLiteral("This image is no longer available");
    emit reviewChanged();
    return;
  }

  const auto& record = m_model->recordAt(row);
  const Candidate candidate{record.path, record.modified, record.bytes};
  m_reviewModified = candidate.modified;
  m_reviewBytes = candidate.bytes;
  if (refresh) {
    m_entries.remove(candidate.path);
    m_failed.remove(failureKey(candidate));
    QFile::remove(cachePath(candidate.path));
    emit textReady(candidate.path, {});
  } else {
    const auto known = m_entries.constFind(candidate.path);
    if (known != m_entries.cend() && known->modified == candidate.modified &&
        known->bytes == candidate.bytes) {
      publishReview(candidate, known->text);
      return;
    }
    QString cached;
    if (readCache(candidate, &cached)) {
      adopt(candidate, cached);
      publishReview(candidate, cached);
      return;
    }
  }

  if (m_current && m_current->path == candidate.path &&
      m_current->modified == candidate.modified && m_current->bytes == candidate.bytes) {
    return;
  }
  m_reviewCandidate = candidate;
  processNext();
}

void OcrIndex::cancelReview() {
  m_reviewCandidate.reset();
  if (m_reviewPath.isEmpty() && m_reviewText.isEmpty() && m_reviewError.isEmpty() &&
      !m_reviewing) {
    return;
  }
  m_reviewPath.clear();
  m_reviewModified = 0;
  m_reviewBytes = 0;
  m_reviewText.clear();
  m_reviewError.clear();
  m_reviewing = false;
  emit reviewChanged();
}

int OcrIndex::clearCache() {
  // Clearing is also an explicit stop. The next edit to an active query can
  // start a fresh index, but this click must not immediately recreate what it
  // just removed.
  m_active = false;
  m_queue.clear();
  m_timeout.stop();
  m_current.reset();
  m_reviewCandidate.reset();
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
  }
  setIndexing(false);
  resetProgress(0);

  const int remembered = m_entries.size();
  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
    emit textReady(it.key(), {});
  }
  m_entries.clear();
  m_failed.clear();
  cancelReview();

  int removed = 0;
  QDir directory(cacheDirectory());
  for (const QString& fileName :
       directory.entryList({QStringLiteral("*.ocr")}, QDir::Files)) {
    removed += QFile::remove(directory.filePath(fileName)) ? 1 : 0;
  }
  return qMax(remembered, removed);
}

void OcrIndex::sync() {
  if (!m_active || !m_model) {
    return;
  }

  QHash<QString, Candidate> live;
  live.reserve(m_model->rowCount());
  for (int row = 0; row < m_model->rowCount(); ++row) {
    const auto& record = m_model->recordAt(row);
    if (!record.isVideo()) {
      live.insert(record.path, {record.path, record.modified, record.bytes});
    }
  }

  for (auto it = m_entries.begin(); it != m_entries.end();) {
    const auto found = live.constFind(it.key());
    if (found == live.cend() || found->modified != it->modified ||
        found->bytes != it->bytes) {
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
  for (const bool screenshots : {true, false}) {
    for (int row = 0; row < m_model->rowCount(); ++row) {
      const auto& record = m_model->recordAt(row);
      if (record.isVideo() || (record.kind == CaptureRecord::Screenshot) != screenshots) {
        continue;
      }
      const Candidate candidate{record.path, record.modified, record.bytes};
      const auto known = m_entries.constFind(candidate.path);
      if ((known != m_entries.cend() && known->modified == candidate.modified &&
           known->bytes == candidate.bytes) ||
          (m_current && m_current->path == candidate.path) ||
          m_failed.contains(cachePath(candidate.path) + QString::number(candidate.modified) +
                            QLatin1Char(':') + QString::number(candidate.bytes))) {
        continue;
      }
      m_queue.append(candidate);
    }
  }

  resetProgress(m_queue.size() + (m_current && !m_currentForReview ? 1 : 0));
  setIndexing(m_current.has_value() || !m_queue.isEmpty());
  processNext();
}

void OcrIndex::processNext() {
  if (m_process.state() != QProcess::NotRunning || m_current.has_value()) {
    return;
  }
  if (!m_reviewCandidate && (!m_active || m_queue.isEmpty())) {
    setIndexing(false);
    return;
  }

  const bool forReview = m_reviewCandidate.has_value();
  const Candidate candidate = forReview ? *m_reviewCandidate : m_queue.takeFirst();
  if (forReview) {
    m_reviewCandidate.reset();
  }
  if (!stillCurrent(candidate)) {
    if (forReview && candidate.path == m_reviewPath) {
      publishReview(candidate, {}, QStringLiteral("This image is no longer available"));
    } else if (!forReview) {
      advanceProgress();
    }
    QTimer::singleShot(0, this, &OcrIndex::processNext);
    return;
  }

  QString cached;
  if (readCache(candidate, &cached)) {
    adopt(candidate, cached);
    if (candidate.path == m_reviewPath) {
      publishReview(candidate, cached);
    }
    if (!forReview) {
      advanceProgress();
    }
    QTimer::singleShot(0, this, &OcrIndex::processNext);
    return;
  }

  if (!forReview && m_failed.contains(failureKey(candidate))) {
    advanceProgress();
    QTimer::singleShot(0, this, &OcrIndex::processNext);
    return;
  }

  m_current = candidate;
  m_currentForReview = forReview;
  m_process.setProgram(m_program);
  m_process.setArguments({candidate.path, QStringLiteral("stdout"),
                          QStringLiteral("--oem"), QStringLiteral("1"),
                          QStringLiteral("-l"), m_languages,
                          QStringLiteral("--dpi"), QStringLiteral("300"),
                          QStringLiteral("-c"),
                          QStringLiteral("preserve_interword_spaces=1")});
  m_process.setProcessChannelMode(QProcess::SeparateChannels);
  m_process.start(QIODevice::ReadOnly);
  m_timeout.start();
}

void OcrIndex::finishCurrent(bool successful) {
  if (!m_current) {
    if (m_active) {
      QTimer::singleShot(0, this, &OcrIndex::processNext);
    }
    return;
  }
  m_timeout.stop();
  const Candidate candidate = *m_current;
  const bool forReview = m_currentForReview;
  m_current.reset();
  m_currentForReview = false;

  if (successful && stillCurrent(candidate)) {
    const QString text = layoutText(m_process.readAllStandardOutput());
    writeCache(candidate, text);
    adopt(candidate, text);
    if (candidate.path == m_reviewPath) {
      publishReview(candidate, text);
    }
  } else {
    m_failed.insert(failureKey(candidate));
    if (candidate.path == m_reviewPath) {
      publishReview(candidate, {}, QStringLiteral("Could not extract text"));
    }
  }
  if (!forReview) {
    advanceProgress();
  }

  if (!m_active && !m_reviewCandidate) {
    setIndexing(false);
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

bool OcrIndex::stillCurrent(const Candidate& candidate) const {
  if (!m_model) {
    return false;
  }
  const int row = m_model->rowOf(candidate.path);
  if (row < 0) {
    return false;
  }
  const auto& record = m_model->recordAt(row);
  return !record.isVideo() && record.modified == candidate.modified &&
         record.bytes == candidate.bytes;
}

QString OcrIndex::cachePath(const QString& path) const {
  const QByteArray key = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256)
                             .toHex();
  return cacheDirectory() + QLatin1Char('/') + QString::fromLatin1(key) +
         QStringLiteral(".ocr");
}

bool OcrIndex::readCache(const Candidate& candidate, QString* text) const {
  QFile file(cachePath(candidate.path));
  if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumCacheEntryBytes) {
    return false;
  }
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  quint32 magic = 0;
  quint16 version = 0;
  qint64 modified = 0;
  qint64 bytes = 0;
  QString path;
  QString stored;
  stream >> magic >> version >> path >> modified >> bytes >> stored;
  if (stream.status() != QDataStream::Ok || magic != kCacheMagic ||
      version != kCacheVersion || path != candidate.path || modified != candidate.modified ||
      bytes != candidate.bytes || stored.size() > kMaximumCacheEntryBytes) {
    return false;
  }
  *text = stored;
  return true;
}

void OcrIndex::writeCache(const Candidate& candidate, const QString& text) const {
  const QString directory = cacheDirectory();
  if (!QDir().mkpath(directory)) {
    return;
  }
  QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner);

  QSaveFile file(cachePath(candidate.path));
  if (!file.open(QIODevice::WriteOnly)) {
    return;
  }
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << kCacheMagic << kCacheVersion << candidate.path << candidate.modified
         << candidate.bytes << text;
  if (stream.status() == QDataStream::Ok && file.commit()) {
    QFile::setPermissions(cachePath(candidate.path),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
}

void OcrIndex::pruneCache() {
  QDir directory(cacheDirectory());
  const QFileInfoList files = directory.entryInfoList(
      {QStringLiteral("*.ocr")}, QDir::Files, QDir::Time);
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
      QString text;
      stream >> magic >> version >> path >> modified >> bytes >> text;
      const QFileInfo source(path);
      keep = stream.status() == QDataStream::Ok && magic == kCacheMagic &&
             version == kCacheVersion && !path.isEmpty() && source.isFile() &&
             source.size() == bytes &&
             source.lastModified().toMSecsSinceEpoch() == modified &&
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
  m_entries.insert(candidate.path, {candidate.modified, candidate.bytes, text});
  emit textReady(candidate.path, normalizedText(text));
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

QString OcrIndex::failureKey(const Candidate& candidate) const {
  return cachePath(candidate.path) + QString::number(candidate.modified) +
         QLatin1Char(':') + QString::number(candidate.bytes);
}
