#include "library/SimilarityIndex.h"

#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>

namespace {
constexpr int kMaximumDistance = 7;
constexpr int kProgressBatchSize = 16;

int findRoot(QList<int>& parent, int value) {
  while (parent[value] != value) {
    parent[value] = parent[parent[value]];
    value = parent[value];
  }
  return value;
}

void join(QList<int>& parent, int left, int right) {
  left = findRoot(parent, left);
  right = findRoot(parent, right);
  if (left != right) {
    parent[right] = left;
  }
}

bool similarAspect(const auto& left, const auto& right) {
  if (left.width <= 0 || left.height <= 0 || right.width <= 0 || right.height <= 0) {
    return false;
  }
  const double a = static_cast<double>(left.width) / left.height;
  const double b = static_cast<double>(right.width) / right.height;
  return std::abs(a - b) / std::max(a, b) <= 0.08;
}

bool similarColour(const auto& left, const auto& right) {
  return std::abs(left.red - right.red) + std::abs(left.green - right.green) +
             std::abs(left.blue - right.blue) <=
         90;
}
} // namespace

SimilarityIndex::SimilarityIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model), m_cancel(std::make_shared<std::atomic_bool>(false)) {
  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(180);
  connect(&m_refreshTimer, &QTimer::timeout, this, &SimilarityIndex::start);
  connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
    const Result result = m_watcher.future().takeResult();
    if (result.generation == m_generation && m_active && !m_restartQueued && !m_dirty) {
      m_cache = result.cache;
      if (m_groups != result.groups) {
        m_groups = result.groups;
        emit groupsChanged();
      }
    }
    setScanning(false);
    if (m_active && (m_restartQueued || m_dirty)) {
      m_restartQueued = false;
      QTimer::singleShot(0, this, &SimilarityIndex::start);
    }
  });
  if (m_model) {
    const auto dirty = [this] { markDirty(); };
    connect(m_model, &QAbstractItemModel::rowsInserted, this, dirty);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, dirty);
    connect(m_model, &QAbstractItemModel::modelReset, this, dirty);
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

SimilarityIndex::~SimilarityIndex() {
  m_cancel->store(true);
  m_watcher.waitForFinished();
}

int SimilarityIndex::groupCount() const {
  return QSet<QString>(m_groups.cbegin(), m_groups.cend()).size();
}

void SimilarityIndex::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  emit activeChanged();
  if (!active) {
    m_refreshTimer.stop();
    if (m_watcher.isRunning()) {
      ++m_generation;
      m_dirty = true;
      m_cancel->store(true);
    }
    setScanning(false);
    setProgress(0, 0);
  } else if (m_dirty) {
    start();
  }
}

void SimilarityIndex::refresh() {
  m_cache.clear();
  m_dirty = true;
  if (m_active) {
    m_refreshTimer.start();
  }
}

void SimilarityIndex::markDirty() {
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

QList<SimilarityIndex::Candidate> SimilarityIndex::candidates() const {
  QList<Candidate> result;
  if (!m_model) {
    return result;
  }
  for (int row = 0; row < m_model->rowCount(); ++row) {
    const CaptureRecord& record = m_model->recordAt(row);
    if (!record.isVideo() && !record.isDocument() && record.bytes > 0) {
      result.append({record.path, record.bytes, record.modified});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const Candidate& left, const Candidate& right) { return left.path < right.path; });
  return result;
}

SimilarityIndex::CachedHash SimilarityIndex::hashFile(const Candidate& candidate) {
  CachedHash result{candidate.bytes, candidate.modified};
  QImageReader reader(candidate.path);
  reader.setAutoTransform(true);
  const QSize original = reader.size();
  if (!original.isValid() || original.isEmpty()) {
    return result;
  }
  reader.setScaledSize(QSize(9, 8));
  const QImage colour = reader.read().convertToFormat(QImage::Format_RGB32);
  if (colour.width() != 9 || colour.height() != 8) {
    return result;
  }
  qint64 red = 0;
  qint64 green = 0;
  qint64 blue = 0;
  for (int y = 0; y < colour.height(); ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(colour.constScanLine(y));
    for (int x = 0; x < colour.width(); ++x) {
      red += qRed(line[x]);
      green += qGreen(line[x]);
      blue += qBlue(line[x]);
    }
  }
  const int pixels = colour.width() * colour.height();
  result.red = int(red / pixels);
  result.green = int(green / pixels);
  result.blue = int(blue / pixels);
  const QImage image = colour.convertToFormat(QImage::Format_Grayscale8);
  quint64 hash = 0;
  int bit = 0;
  for (int y = 0; y < 8; ++y) {
    const uchar* line = image.constScanLine(y);
    for (int x = 0; x < 8; ++x) {
      if (line[x] > line[x + 1]) {
        hash |= quint64(1) << bit;
      }
      ++bit;
    }
  }
  const QFileInfo current(candidate.path);
  if (!current.isFile() || current.size() != candidate.bytes ||
      current.lastModified().toMSecsSinceEpoch() != candidate.modified) {
    return result;
  }
  result.hash = hash;
  result.width = original.width();
  result.height = original.height();
  result.valid = true;
  return result;
}

void SimilarityIndex::start() {
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
  if (work.size() < 2) {
    m_groups.clear();
    emit groupsChanged();
    setProgress(0, 0);
    return;
  }
  m_cancel = std::make_shared<std::atomic_bool>(false);
  const auto cancel = m_cancel;
  const auto cache = m_cache;
  const quint64 generation = m_generation;
  const QPointer<SimilarityIndex> guard(this);
  setProgress(0, work.size());
  setScanning(true);

  m_watcher.setFuture(QtConcurrent::run([work, cache, cancel, generation, guard] {
    Result result;
    result.generation = generation;
    QList<CachedHash> hashes;
    hashes.reserve(work.size());
    int completed = 0;
    for (const Candidate& candidate : work) {
      if (cancel->load()) {
        return result;
      }
      CachedHash hash = cache.value(candidate.path);
      if (hash.bytes != candidate.bytes || hash.modified != candidate.modified) {
        hash = hashFile(candidate);
      }
      hashes.append(hash);
      if (hash.valid) {
        result.cache.insert(candidate.path, hash);
      }
      ++completed;
      if (guard && (completed == work.size() || completed % kProgressBatchSize == 0)) {
        QMetaObject::invokeMethod(guard.data(), [guard, generation, completed] {
          if (guard && guard->m_generation == generation && guard->m_active) {
            guard->setProgress(completed, guard->m_total);
          }
        }, Qt::QueuedConnection);
      }
    }

    QList<int> parent(work.size());
    std::iota(parent.begin(), parent.end(), 0);
    QHash<quint64, int> exactRepresentative;
    QMultiHash<int, int> chunks;
    for (int index = 0; index < hashes.size(); ++index) {
      if (cancel->load() || !hashes[index].valid) {
        continue;
      }
      const auto exact = exactRepresentative.constFind(hashes[index].hash);
      if (exact != exactRepresentative.cend() &&
          similarAspect(hashes[index], hashes[exact.value()]) &&
          similarColour(hashes[index], hashes[exact.value()])) {
        join(parent, index, exact.value());
        continue;
      }

      QSet<int> possible;
      for (int chunk = 0; chunk < 8; ++chunk) {
        const int key = chunk * 256 + int((hashes[index].hash >> (chunk * 8)) & 0xff);
        const auto values = chunks.values(key);
        for (int value : values) {
          possible.insert(value);
        }
      }
      for (int other : std::as_const(possible)) {
        if (similarAspect(hashes[index], hashes[other]) &&
            similarColour(hashes[index], hashes[other]) &&
            std::popcount(hashes[index].hash ^ hashes[other].hash) <= kMaximumDistance) {
          join(parent, index, other);
        }
      }
      exactRepresentative.insert(hashes[index].hash, index);
      for (int chunk = 0; chunk < 8; ++chunk) {
        const int key = chunk * 256 + int((hashes[index].hash >> (chunk * 8)) & 0xff);
        chunks.insert(key, index);
      }
    }

    QHash<int, QStringList> sets;
    for (int index = 0; index < work.size(); ++index) {
      if (hashes[index].valid) {
        sets[findRoot(parent, index)].append(work[index].path);
      }
    }
    for (auto it = sets.cbegin(); it != sets.cend(); ++it) {
      if (it.value().size() < 2) {
        continue;
      }
      const QString group = QStringLiteral("similar:%1").arg(it.key());
      for (const QString& path : it.value()) {
        result.groups.insert(path, group);
      }
    }
    return result;
  }));
}

void SimilarityIndex::setScanning(bool scanning) {
  if (m_scanning != scanning) {
    m_scanning = scanning;
    emit scanningChanged();
  }
}

void SimilarityIndex::setProgress(int completed, int total) {
  if (m_completed != completed || m_total != total) {
    m_completed = qMax(0, completed);
    m_total = qMax(0, total);
    emit progressChanged();
  }
}
