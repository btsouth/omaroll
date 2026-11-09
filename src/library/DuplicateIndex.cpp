#include "library/DuplicateIndex.h"

#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "library/HashCache.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {

constexpr qint64 kHashChunkBytes = 1024LL * 1024;
constexpr int kProgressBatchSize = 32;
constexpr quint32 kCacheMagic = 0x4f444931; // ODI1
constexpr quint16 kCacheVersion = 1;
constexpr qint64 kMaximumCacheBytes = 16LL * 1024 * 1024;

} // namespace

DuplicateIndex::DuplicateIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model), m_cancel(std::make_shared<std::atomic_bool>(false)) {
  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(180);
  connect(&m_refreshTimer, &QTimer::timeout, this, &DuplicateIndex::start);

  connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
    const Result result = m_watcher.future().takeResult();
    const bool current = result.generation == m_generation && m_active && !m_restartQueued &&
                         !m_dirty && result.complete;
    if (current) {
      m_cache = result.cache;
      m_diskLoaded = true;
      m_lastHashed = result.hashed;
      if (m_groups != result.groups) {
        m_groups = result.groups;
        emit groupsChanged();
      }
    }

    setScanning(false);
    if (m_active && (m_restartQueued || m_dirty)) {
      m_restartQueued = false;
      QTimer::singleShot(0, this, &DuplicateIndex::start);
    }
  });

  if (m_model) {
    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this] { markDirty(); });
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, [this] { markDirty(); });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] { markDirty(); });
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
              if (roles.isEmpty() || roles.contains(CaptureRoles::PathRole) ||
                  roles.contains(CaptureRoles::BytesRole) ||
                  roles.contains(CaptureRoles::StampRole)) {
                markDirty();
              }
            });
  }
}

DuplicateIndex::~DuplicateIndex() {
  requestStop();
  m_watcher.waitForFinished();
}

QString DuplicateIndex::cachePath() { return HashCache::path(QStringLiteral("duplicates.index")); }

int DuplicateIndex::groupCount() const {
  QSet<QString> unique;
  for (auto it = m_groups.cbegin(); it != m_groups.cend(); ++it) {
    unique.insert(it.value());
  }
  return unique.size();
}

QStringList DuplicateIndex::groupPaths(const QString& path) const {
  const QString group = m_groups.value(path);
  if (group.isEmpty()) {
    return {};
  }
  QStringList paths;
  for (auto it = m_groups.cbegin(); it != m_groups.cend(); ++it) {
    if (it.value() == group) {
      paths.append(it.key());
    }
  }
  paths.sort(Qt::CaseSensitive);
  return paths;
}

QStringList DuplicateIndex::otherCopies(const QString& keepPath) const {
  if (!ready()) return {};
  QStringList paths = groupPaths(keepPath);
  paths.removeAll(keepPath);
  return paths;
}

void DuplicateIndex::requestStop() {
  m_refreshTimer.stop();
  m_active = false;
  m_restartQueued = false;
  ++m_generation;
  m_cancel->store(true);
}

void DuplicateIndex::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  emit activeChanged();
  emit readinessChanged();

  if (!m_active) {
    m_refreshTimer.stop();
    m_restartQueued = false;
    if (m_watcher.isRunning()) {
      m_dirty = true;
      ++m_generation;
      m_cancel->store(true);
    }
    setScanning(false);
    setProgress(0, 0);
    return;
  }

  if (m_dirty) {
    start();
  }
}

void DuplicateIndex::refresh() {
  // An explicit recheck must read content again even if a caller preserved a
  // file's size and timestamp while rewriting it, so the disk cache is
  // skipped too.
  m_cache.clear();
  m_diskLoaded = true;
  markDirty();
}

void DuplicateIndex::markDirty() {
  m_dirty = true;
  emit readinessChanged();
  if (!m_active) {
    return;
  }
  if (m_watcher.isRunning()) {
    m_cancel->store(true);
    m_restartQueued = true;
  }
  m_refreshTimer.start();
}

QList<DuplicateIndex::Candidate> DuplicateIndex::candidates() const {
  QHash<qint64, QList<Candidate>> bySize;
  if (!m_model) {
    return {};
  }
  for (int row = 0; row < m_model->rowCount(); ++row) {
    const CaptureRecord& record = m_model->recordAt(row);
    // Empty files are not useful media and should not fill a review view just
    // because several interrupted writes left the same zero-byte artifact.
    if (record.bytes > 0) {
      bySize[record.bytes].append({record.path, record.bytes, record.modified});
    }
  }

  QList<Candidate> result;
  for (auto it = bySize.cbegin(); it != bySize.cend(); ++it) {
    if (it.value().size() > 1) {
      result.append(it.value());
    }
  }
  std::sort(result.begin(), result.end(), [](const Candidate& first, const Candidate& second) {
    return first.path < second.path;
  });
  return result;
}

QByteArray DuplicateIndex::hashFile(const Candidate& candidate, const std::atomic_bool* cancel) {
  QFile file(candidate.path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }

  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    if (cancel && cancel->load()) {
      return {};
    }
    const QByteArray chunk = file.read(kHashChunkBytes);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
      return {};
    }
    hash.addData(chunk);
  }

  const QFileInfo current(candidate.path);
  if (!current.isFile() || current.size() != candidate.bytes ||
      current.lastModified().toMSecsSinceEpoch() != candidate.modified) {
    return {};
  }
  return hash.result();
}

void DuplicateIndex::start() {
  if (!m_active || !m_model) {
    return;
  }
  if (m_watcher.isRunning()) {
    m_cancel->store(true);
    m_restartQueued = true;
    return;
  }

  const QList<Candidate> work = candidates();
  m_dirty = false;
  m_restartQueued = false;
  ++m_generation;

  if (work.isEmpty()) {
    if (!m_groups.isEmpty()) {
      m_groups.clear();
      emit groupsChanged();
    }
    setProgress(0, 0);
    setScanning(false);
    emit readinessChanged();
    return;
  }

  m_cancel = std::make_shared<std::atomic_bool>(false);
  const auto cancel = m_cancel;
  const auto cache = m_cache;
  const bool loadDisk = !m_diskLoaded;
  const quint64 generation = m_generation;
  const QPointer<DuplicateIndex> guard(this);
  setProgress(0, work.size());
  setScanning(true);

  m_watcher.setFuture(QtConcurrent::run([work, cache, loadDisk, cancel, generation, guard] {
    Result result;
    result.generation = generation;
    result.cache = cache;
    qsizetype loaded = 0;
    if (loadDisk) {
      const HashCache::Entries disk = HashCache::load(cachePath(), kCacheMagic, kCacheVersion,
                                                      kMaximumCacheBytes, cancel.get());
      loaded = disk.size();
      for (auto it = disk.cbegin(); it != disk.cend(); ++it) {
        if (!result.cache.contains(it.key()) && it->data.size() == 32) {
          result.cache.insert(it.key(), {it->bytes, it->modified, it->data});
        }
      }
    }
    QHash<QString, QList<QString>> contentGroups;
    int completed = 0;

    for (const Candidate& candidate : work) {
      if (cancel->load()) {
        return result;
      }

      QByteArray digest;
      const auto cached = result.cache.constFind(candidate.path);
      if (cached != result.cache.cend() && cached->bytes == candidate.bytes &&
          cached->modified == candidate.modified) {
        digest = cached->digest;
      } else {
        digest = hashFile(candidate, cancel.get());
        ++result.hashed;
      }
      if (cancel->load()) {
        return result;
      }
      if (digest.isEmpty()) {
        result.cache.remove(candidate.path);
      } else {
        result.cache.insert(candidate.path, {candidate.bytes, candidate.modified, digest});
        const QString key = QString::number(candidate.bytes) + QLatin1Char(':') +
                            QString::fromLatin1(digest.toHex());
        contentGroups[key].append(candidate.path);
      }

      ++completed;
      if (guard && (completed == work.size() || completed % kProgressBatchSize == 0)) {
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, generation, completed] {
              if (guard && guard->m_generation == generation && guard->m_active) {
                guard->setProgress(completed, guard->m_total);
              }
            },
            Qt::QueuedConnection);
      }
    }

    for (auto it = contentGroups.cbegin(); it != contentGroups.cend(); ++it) {
      if (it.value().size() < 2) {
        continue;
      }
      for (const QString& path : it.value()) {
        result.groups.insert(path, it.key());
      }
    }
    if (result.hashed > 0 || (loadDisk && loaded != result.cache.size())) {
      HashCache::Entries entries;
      entries.reserve(result.cache.size());
      for (auto it = result.cache.cbegin(); it != result.cache.cend(); ++it) {
        entries.insert(it.key(), {it->bytes, it->modified, it->digest});
      }
      QList<QString> live;
      live.reserve(work.size());
      for (const Candidate& candidate : work) live.append(candidate.path);
      HashCache::save(cachePath(), kCacheMagic, kCacheVersion, entries, live, kMaximumCacheBytes);
    }
    result.complete = true;
    return result;
  }));
}

void DuplicateIndex::setScanning(bool scanning) {
  if (m_scanning == scanning) {
    return;
  }
  m_scanning = scanning;
  emit scanningChanged();
  emit readinessChanged();
}

void DuplicateIndex::setProgress(int completed, int total) {
  completed = qMax(0, completed);
  total = qMax(0, total);
  if (m_completed == completed && m_total == total) {
    return;
  }
  m_completed = completed;
  m_total = total;
  emit progressChanged();
}
