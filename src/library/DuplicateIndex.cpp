#include "library/DuplicateIndex.h"

#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {

constexpr qint64 kHashChunkBytes = 1024 * 1024;

} // namespace

DuplicateIndex::DuplicateIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model),
      m_cancel(std::make_shared<std::atomic_bool>(false)) {
  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(180);
  connect(&m_refreshTimer, &QTimer::timeout, this, &DuplicateIndex::start);

  connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
    const Result result = m_watcher.future().takeResult();
    const bool current = result.generation == m_generation && m_active &&
                         !m_restartQueued && !m_dirty;
    if (current) {
      m_cache = result.cache;
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
    connect(m_model, &QAbstractItemModel::rowsInserted, this,
            [this] { markDirty(); });
    connect(m_model, &QAbstractItemModel::rowsRemoved, this,
            [this] { markDirty(); });
    connect(m_model, &QAbstractItemModel::modelReset, this,
            [this] { markDirty(); });
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
  m_cancel->store(true);
  m_watcher.waitForFinished();
}

int DuplicateIndex::groupCount() const {
  QSet<QString> unique;
  for (auto it = m_groups.cbegin(); it != m_groups.cend(); ++it) {
    unique.insert(it.value());
  }
  return unique.size();
}

void DuplicateIndex::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  emit activeChanged();

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
  // file's size and timestamp while rewriting it.
  m_cache.clear();
  m_dirty = true;
  if (m_active) {
    m_refreshTimer.start();
  }
}

void DuplicateIndex::markDirty() {
  m_dirty = true;
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
  std::sort(result.begin(), result.end(),
            [](const Candidate& first, const Candidate& second) {
              return first.path < second.path;
            });
  return result;
}

QByteArray DuplicateIndex::hashFile(const Candidate& candidate,
                                    const std::atomic_bool* cancel) {
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
    m_cache.clear();
    if (!m_groups.isEmpty()) {
      m_groups.clear();
      emit groupsChanged();
    }
    setProgress(0, 0);
    setScanning(false);
    return;
  }

  m_cancel = std::make_shared<std::atomic_bool>(false);
  const auto cancel = m_cancel;
  const auto cache = m_cache;
  const quint64 generation = m_generation;
  const QPointer<DuplicateIndex> guard(this);
  setProgress(0, work.size());
  setScanning(true);

  m_watcher.setFuture(QtConcurrent::run([work, cache, cancel, generation, guard] {
    Result result;
    result.generation = generation;
    QHash<QString, QList<QString>> contentGroups;
    int completed = 0;

    for (const Candidate& candidate : work) {
      if (cancel->load()) {
        return result;
      }

      QByteArray digest;
      const auto cached = cache.constFind(candidate.path);
      if (cached != cache.cend() && cached->bytes == candidate.bytes &&
          cached->modified == candidate.modified) {
        digest = cached->digest;
      } else {
        digest = hashFile(candidate, cancel.get());
      }
      if (cancel->load()) {
        return result;
      }
      if (!digest.isEmpty()) {
        result.cache.insert(candidate.path,
                            {candidate.bytes, candidate.modified, digest});
        const QString key = QString::number(candidate.bytes) + QLatin1Char(':') +
                            QString::fromLatin1(digest.toHex());
        contentGroups[key].append(candidate.path);
      }

      ++completed;
      if (guard) {
        QMetaObject::invokeMethod(
            guard.data(), [guard, generation, completed] {
              if (guard && guard->m_generation == generation && guard->m_active) {
                guard->setProgress(completed, guard->m_total);
              }
            }, Qt::QueuedConnection);
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
    return result;
  }));
}

void DuplicateIndex::setScanning(bool scanning) {
  if (m_scanning == scanning) {
    return;
  }
  m_scanning = scanning;
  emit scanningChanged();
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
