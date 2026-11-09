#include "library/SimilarityIndex.h"

#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "library/HashCache.h"
#include "sources/CameraRaw.h"
#include "sources/ImageBudget.h"

#include <QDataStream>
#include <QFileInfo>
#include <QIODevice>
#include <QImageReader>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace {

constexpr int kProgressBatchSize = 16;
constexpr int kSample = 64;
constexpr int kThumbnail = 32;
constexpr int kThumbnailPixels = kThumbnail * kThumbnail;

constexpr quint32 kCacheMagic = 0x4f534931; // OSI1
constexpr quint16 kCacheVersion = 1;
constexpr qint64 kMaximumCacheBytes = 64LL * 1024 * 1024;

// Per strictness: Strict, Normal, Loose.
struct Limits {
  int hashDistance;
  double correlation;
  int brightness;
  int tint;
  double words;
};
// Photos and other colourful pictures. Bursts and edits of one scene belong
// together at Normal; Loose reaches recrops and heavier edits.
constexpr std::array<Limits, 3> kPicture = {{
    {5, 0.95, 45, 30, 0.9},
    {7, 0.90, 70, 45, 0.8},
    {10, 0.84, 100, 70, 0.65},
}};
// Screenshots of text. Two terminals or documents share their layout, so only
// a near-identical thumbnail counts, and recognized text must agree.
constexpr std::array<Limits, 3> kTextLike = {{
    {3, 0.99, 30, 20, 0.95},
    {5, 0.975, 50, 30, 0.85},
    {7, 0.95, 80, 45, 0.7},
}};
// Below this many recognized words text is not used to decide.
constexpr int kMinimumWords = 3;

bool similarAspect(const SimilarityIndex::Signature& left, const SimilarityIndex::Signature& right) {
  if (left.width <= 0 || left.height <= 0 || right.width <= 0 || right.height <= 0) {
    return false;
  }
  const double a = static_cast<double>(left.width) / left.height;
  const double b = static_cast<double>(right.width) / right.height;
  return std::abs(a - b) / std::max(a, b) <= 0.08;
}

double variance(const SimilarityIndex::Signature& signature) {
  const double mean = double(signature.sum) / kThumbnailPixels;
  return double(signature.sumSquares) / kThumbnailPixels - mean * mean;
}

double correlation(const SimilarityIndex::Signature& left, const SimilarityIndex::Signature& right) {
  const auto* a = reinterpret_cast<const uchar*>(left.thumbnail.constData());
  const auto* b = reinterpret_cast<const uchar*>(right.thumbnail.constData());
  qint64 product = 0;
  for (int index = 0; index < kThumbnailPixels; ++index) {
    product += int(a[index]) * int(b[index]);
  }
  const double n = kThumbnailPixels;
  const double covariance = double(product) / n - (double(left.sum) / n) * (double(right.sum) / n);
  const double spread = std::sqrt(variance(left) * variance(right));
  return spread > 0 ? covariance / spread : 0;
}

double wordAgreement(const QStringList& left, const QStringList& right) {
  // Both lists are sorted and unique.
  int shared = 0;
  int i = 0;
  int j = 0;
  while (i < left.size() && j < right.size()) {
    const int order = left[i].compare(right[j]);
    if (order == 0) {
      ++shared;
      ++i;
      ++j;
    } else if (order < 0) {
      ++i;
    } else {
      ++j;
    }
  }
  const int combined = int(left.size() + right.size()) - shared;
  return combined > 0 ? double(shared) / combined : 1.0;
}

int findSet(QList<int>& sets, int value) {
  while (sets[value] != value) {
    sets[value] = sets[sets[value]];
    value = sets[value];
  }
  return value;
}

quint64 pairKey(int left, int right) {
  return (quint64(quint32(std::min(left, right))) << 32) | quint32(std::max(left, right));
}

} // namespace

void SimilarityIndex::Signature::prepare() {
  sum = 0;
  sumSquares = 0;
  if (thumbnail.size() != kThumbnailPixels) {
    valid = false;
    return;
  }
  const auto* pixels = reinterpret_cast<const uchar*>(thumbnail.constData());
  for (int index = 0; index < kThumbnailPixels; ++index) {
    sum += pixels[index];
    sumSquares += int(pixels[index]) * int(pixels[index]);
  }
}

bool SimilarityIndex::Signature::textLike() const {
  // Most of the picture one flat shade and almost no colour: a terminal, a
  // document, a chat or a settings window. Photographs rarely are both.
  return flatness >= 150 && chroma <= 30;
}

QByteArray SimilarityIndex::Signature::serialize() const {
  QByteArray data;
  QDataStream stream(&data, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << hash << qint32(width) << qint32(height) << quint8(red) << quint8(green)
         << quint8(blue) << quint8(flatness) << quint8(chroma) << thumbnail;
  return data;
}

SimilarityIndex::Signature SimilarityIndex::Signature::deserialize(qint64 bytes, qint64 modified,
                                                                   const QByteArray& data) {
  Signature signature;
  signature.bytes = bytes;
  signature.modified = modified;
  QDataStream stream(data);
  stream.setVersion(QDataStream::Qt_6_0);
  qint32 width = 0;
  qint32 height = 0;
  quint8 red = 0;
  quint8 green = 0;
  quint8 blue = 0;
  quint8 flatness = 0;
  quint8 chroma = 0;
  stream >> signature.hash >> width >> height >> red >> green >> blue >> flatness >> chroma >>
      signature.thumbnail;
  if (stream.status() != QDataStream::Ok || width <= 0 || height <= 0 ||
      signature.thumbnail.size() != kThumbnailPixels) {
    return Signature{};
  }
  signature.width = width;
  signature.height = height;
  signature.red = red;
  signature.green = green;
  signature.blue = blue;
  signature.flatness = flatness;
  signature.chroma = chroma;
  signature.valid = true;
  signature.prepare();
  return signature;
}

QStringList SimilarityIndex::wordsOf(const QString& text) {
  QSet<QString> words;
  QString word;
  const auto flush = [&] {
    if (word.size() >= 3) {
      words.insert(word);
    }
    word.clear();
  };
  for (const QChar character : text) {
    if (character.isLetterOrNumber()) {
      word.append(character.toCaseFolded());
    } else {
      flush();
    }
  }
  flush();
  QStringList sorted(words.cbegin(), words.cend());
  sorted.sort();
  return sorted;
}

bool SimilarityIndex::matches(const Signature& left, const Signature& right,
                              const QStringList& leftWords, const QStringList& rightWords,
                              int strictness, double* score) {
  if (!left.valid || !right.valid || !similarAspect(left, right)) {
    return false;
  }
  const int level = std::clamp(strictness, 0, 2);
  const bool text = left.textLike() || right.textLike();
  const Limits& limits = text ? kTextLike[level] : kPicture[level];
  if (std::popcount(left.hash ^ right.hash) > limits.hashDistance) {
    return false;
  }

  // Brightness may change; the balance between the channels may not much.
  const int leftLuma = (left.red + left.green + left.blue) / 3;
  const int rightLuma = (right.red + right.green + right.blue) / 3;
  const int tint = std::abs((left.red - leftLuma) - (right.red - rightLuma)) +
                   std::abs((left.green - leftLuma) - (right.green - rightLuma)) +
                   std::abs((left.blue - leftLuma) - (right.blue - rightLuma));
  if (std::abs(leftLuma - rightLuma) > limits.brightness || tint > limits.tint) {
    return false;
  }

  if (leftWords.size() >= kMinimumWords && rightWords.size() >= kMinimumWords &&
      wordAgreement(leftWords, rightWords) < limits.words) {
    return false;
  }

  // A picture of one flat colour has no structure to correlate; it matches
  // only another of nearly the same shade.
  const bool leftFlat = variance(left) < 4.0;
  const bool rightFlat = variance(right) < 4.0;
  double closeness = 0;
  if (leftFlat || rightFlat) {
    if (!leftFlat || !rightFlat ||
        std::abs(double(left.sum - right.sum)) / kThumbnailPixels > 6.0) {
      return false;
    }
    closeness = 1.0;
  } else {
    closeness = correlation(left, right);
    if (closeness < limits.correlation) {
      return false;
    }
  }
  if (score) {
    *score = closeness;
  }
  return true;
}

QList<QList<int>> SimilarityIndex::group(const QList<Signature>& signatures,
                                         const QList<QStringList>& words, int strictness,
                                         const std::atomic_bool* cancel) {
  // Candidate pairs: any two whose difference hashes share one of eight
  // bytes, which every pair within seven bits does. Each is then checked
  // in full.
  struct Edge {
    int left;
    int right;
    double score;
  };
  QList<Edge> edges;
  QSet<quint64> linked;
  QMultiHash<int, int> chunks;
  const QStringList none;
  for (int index = 0; index < signatures.size(); ++index) {
    if (cancel && cancel->load()) return {};
    if (!signatures[index].valid) continue;
    QSet<int> possible;
    for (int chunk = 0; chunk < 8; ++chunk) {
      const int key = chunk * 256 + int((signatures[index].hash >> (chunk * 8)) & 0xff);
      for (auto it = chunks.constFind(key); it != chunks.cend() && it.key() == key; ++it) {
        possible.insert(it.value());
      }
    }
    const QStringList& ownWords = index < words.size() ? words.at(index) : none;
    for (int other : std::as_const(possible)) {
      if (cancel && cancel->load()) return {};
      const QStringList& otherWords = other < words.size() ? words.at(other) : none;
      double score = 0;
      if (matches(signatures[index], signatures[other], ownWords, otherWords, strictness,
                  &score)) {
        edges.append({other, index, score});
        linked.insert(pairKey(other, index));
      }
    }
    for (int chunk = 0; chunk < 8; ++chunk) {
      chunks.insert(chunk * 256 + int((signatures[index].hash >> (chunk * 8)) & 0xff), index);
    }
  }

  // Complete linkage, closest pairs first: two sets merge only when every
  // picture in one matches every picture in the other.
  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.left != b.left ? a.left < b.left : a.right < b.right;
  });
  QList<int> sets(signatures.size());
  QHash<int, QList<int>> members;
  for (int index = 0; index < sets.size(); ++index) {
    sets[index] = index;
  }
  for (const Edge& edge : std::as_const(edges)) {
    if (cancel && cancel->load()) return {};
    const int left = findSet(sets, edge.left);
    const int right = findSet(sets, edge.right);
    if (left == right) continue;
    const QList<int> leftMembers = members.value(left, {left});
    const QList<int> rightMembers = members.value(right, {right});
    bool everyPair = true;
    for (int a : leftMembers) {
      for (int b : rightMembers) {
        if (!linked.contains(pairKey(a, b))) {
          everyPair = false;
          break;
        }
      }
      if (!everyPair) break;
    }
    if (!everyPair) continue;
    sets[right] = left;
    members[left] = leftMembers + rightMembers;
    members.remove(right);
  }
  QList<QList<int>> result;
  for (auto it = members.cbegin(); it != members.cend(); ++it) {
    if (it.value().size() < 2) continue;
    QList<int> set = it.value();
    std::sort(set.begin(), set.end());
    result.append(set);
  }
  std::sort(result.begin(), result.end());
  return result;
}

SimilarityIndex::Signature SimilarityIndex::signatureOf(const QString& path,
                                                        const std::atomic_bool* cancel) {
  Signature result;
  QSize original;
  QImage sample;
  if (CameraRaw::isRawFile(path)) {
    // The raw plugin cannot scale while decoding; its embedded preview is
    // the quick way to the same sample.
    original = CameraRaw::fullSize(path);
    sample = CameraRaw::readPreview(path, QSize(kSample, kSample));
    if (!sample.isNull()) {
      sample = sample.scaled(kSample, kSample, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
  } else {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    original = reader.size();
    if (!original.isValid() || original.isEmpty()) {
      return result;
    }
    if (reader.transformation() & QImageIOHandler::TransformationRotate90) {
      original.transpose();
    }
    reader.setScaledSize(QSize(kSample, kSample));
    // A stop must not wait for another worker's very large decode.
    sample = ImageBudget::readBounded(reader, [cancel] { return cancel && cancel->load(); });
    if (!sample.isNull() && sample.size() != QSize(kSample, kSample)) {
      sample = sample.scaled(kSample, kSample, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
  }
  if (sample.isNull() || !original.isValid() || original.isEmpty()) {
    return result;
  }
  // Transparent pixels count as composited over black, as a viewer shows them
  // on a dark background; keeping only the colour channels would turn every
  // transparent area into whatever colour happened to be stored there.
  QImage colour(kSample, kSample, QImage::Format_RGB32);
  colour.fill(Qt::black);
  {
    QPainter painter(&colour);
    painter.drawImage(0, 0, sample.convertToFormat(QImage::Format_ARGB32_Premultiplied));
  }

  qint64 red = 0;
  qint64 green = 0;
  qint64 blue = 0;
  qint64 chroma = 0;
  std::array<int, 256> histogram{};
  for (int y = 0; y < kSample; ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(colour.constScanLine(y));
    for (int x = 0; x < kSample; ++x) {
      const int r = qRed(line[x]);
      const int g = qGreen(line[x]);
      const int b = qBlue(line[x]);
      red += r;
      green += g;
      blue += b;
      chroma += std::max({r, g, b}) - std::min({r, g, b});
      ++histogram[qGray(line[x])];
    }
  }
  constexpr int samplePixels = kSample * kSample;
  result.red = int(red / samplePixels);
  result.green = int(green / samplePixels);
  result.blue = int(blue / samplePixels);
  result.chroma = int(chroma / samplePixels);
  const int mode = int(std::max_element(histogram.cbegin(), histogram.cend()) - histogram.cbegin());
  int nearMode = 0;
  for (int value = std::max(0, mode - 10); value <= std::min(255, mode + 10); ++value) {
    nearMode += histogram[value];
  }
  result.flatness = nearMode * 255 / samplePixels;

  const QImage difference = colour.scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                .convertToFormat(QImage::Format_Grayscale8);
  int bit = 0;
  for (int y = 0; y < 8; ++y) {
    const uchar* line = difference.constScanLine(y);
    for (int x = 0; x < 8; ++x) {
      if (line[x] > line[x + 1]) {
        result.hash |= quint64(1) << bit;
      }
      ++bit;
    }
  }

  const QImage small = colour.scaled(kThumbnail, kThumbnail, Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
  result.thumbnail.resize(kThumbnailPixels);
  auto* pixels = reinterpret_cast<uchar*>(result.thumbnail.data());
  for (int y = 0; y < kThumbnail; ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(small.constScanLine(y));
    for (int x = 0; x < kThumbnail; ++x) {
      pixels[y * kThumbnail + x] = uchar(qGray(line[x]));
    }
  }
  result.width = original.width();
  result.height = original.height();
  result.valid = true;
  result.prepare();
  return result;
}

SimilarityIndex::SimilarityIndex(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model), m_cancel(std::make_shared<std::atomic_bool>(false)) {
  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(180);
  connect(&m_refreshTimer, &QTimer::timeout, this, &SimilarityIndex::start);
  connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
    const Result result = m_watcher.future().takeResult();
    if (result.generation == m_generation && m_active && !m_restartQueued && !m_dirty &&
        result.complete) {
      m_cache = result.cache;
      m_diskLoaded = true;
      m_lastDecoded = result.decoded;
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
  requestStop();
  m_watcher.waitForFinished();
}

QString SimilarityIndex::cachePath() { return HashCache::path(QStringLiteral("similarity.index")); }

int SimilarityIndex::groupCount() const {
  return QSet<QString>(m_groups.cbegin(), m_groups.cend()).size();
}

QStringList SimilarityIndex::groupPaths(const QString& path) const {
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

void SimilarityIndex::setStrictness(int strictness) {
  strictness = std::clamp(strictness, int(Strict), int(Loose));
  if (m_strictness == strictness) {
    return;
  }
  m_strictness = strictness;
  emit strictnessChanged();
  markDirty();
}

void SimilarityIndex::setText(const QString& path, const QString& text) {
  const QStringList words = wordsOf(text);
  const bool changed = words.isEmpty() ? m_words.remove(path) > 0
                                       : m_words.value(path) != words;
  if (!changed) {
    return;
  }
  if (!words.isEmpty()) {
    m_words.insert(path, words);
  }
  // Text only ever splits a set, so only a picture already in one matters.
  if (m_active && m_groups.contains(path)) {
    markDirty();
  }
}

void SimilarityIndex::requestStop() {
  m_refreshTimer.stop();
  m_active = false;
  m_restartQueued = false;
  ++m_generation;
  m_cancel->store(true);
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
      m_cancel->store(true);
    }
    // The signatures are on disk; release the memory until the view returns.
    m_cache.clear();
    m_diskLoaded = false;
    m_dirty = true;
    setScanning(false);
    setProgress(0, 0);
  } else if (m_dirty) {
    start();
  }
}

void SimilarityIndex::refresh() {
  // An explicit recheck decodes again, ignoring both caches.
  m_cache.clear();
  m_diskLoaded = true;
  m_dirty = true;
  if (m_active) {
    if (m_watcher.isRunning()) {
      m_cancel->store(true);
      m_restartQueued = true;
    }
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
    if (!m_groups.isEmpty()) {
      m_groups.clear();
      emit groupsChanged();
    }
    setProgress(0, 0);
    return;
  }
  m_cancel = std::make_shared<std::atomic_bool>(false);
  const auto cancel = m_cancel;
  const auto cache = m_cache;
  const bool loadDisk = !m_diskLoaded;
  const int strictness = m_strictness;
  QHash<QString, QStringList> words;
  for (const Candidate& candidate : work) {
    const auto found = m_words.constFind(candidate.path);
    if (found != m_words.cend()) {
      words.insert(candidate.path, found.value());
    }
  }
  const quint64 generation = m_generation;
  const QPointer<SimilarityIndex> guard(this);
  setProgress(0, work.size());
  setScanning(true);

  m_watcher.setFuture(QtConcurrent::run([work, cache, loadDisk, strictness, words, cancel,
                                         generation, guard] {
    Result result;
    result.generation = generation;
    result.cache = cache;
    HashCache::Entries disk;
    if (loadDisk) {
      disk = HashCache::load(cachePath(), kCacheMagic, kCacheVersion, kMaximumCacheBytes,
                             cancel.get());
      // Entries for files outside today's library stay on disk as history,
      // in case their folder comes back.
      for (auto it = disk.cbegin(); it != disk.cend(); ++it) {
        if (!result.cache.contains(it.key())) {
          Signature signature =
              Signature::deserialize(it->bytes, it->modified, it->data);
          if (signature.valid) {
            result.cache.insert(it.key(), std::move(signature));
          }
        }
      }
    }
    QList<Signature> signatures;
    signatures.reserve(work.size());
    int completed = 0;
    for (const Candidate& candidate : work) {
      if (cancel->load()) {
        return result;
      }
      Signature signature = result.cache.value(candidate.path);
      if (!signature.valid || signature.bytes != candidate.bytes ||
          signature.modified != candidate.modified) {
        signature = signatureOf(candidate.path, cancel.get());
        signature.bytes = candidate.bytes;
        signature.modified = candidate.modified;
        ++result.decoded;
        const QFileInfo current(candidate.path);
        if (!current.isFile() || current.size() != candidate.bytes ||
            current.lastModified().toMSecsSinceEpoch() != candidate.modified) {
          signature.valid = false;
        }
        if (signature.valid) {
          result.cache.insert(candidate.path, signature);
        } else {
          result.cache.remove(candidate.path);
        }
      }
      signatures.append(signature);
      ++completed;
      if (guard && (completed == work.size() || completed % kProgressBatchSize == 0)) {
        QMetaObject::invokeMethod(guard.data(), [guard, generation, completed] {
          if (guard && guard->m_generation == generation && guard->m_active) {
            guard->setProgress(completed, guard->m_total);
          }
        }, Qt::QueuedConnection);
      }
    }

    QList<QStringList> wordLists;
    wordLists.reserve(work.size());
    for (const Candidate& candidate : work) wordLists.append(words.value(candidate.path));
    const QList<QList<int>> sets = group(signatures, wordLists, strictness, cancel.get());
    if (cancel->load()) return result;
    for (const QList<int>& set : sets) {
      QStringList paths;
      for (int index : set) paths.append(work[index].path);
      paths.sort();
      const QString name = QStringLiteral("similar:") + paths.first();
      for (const QString& path : std::as_const(paths)) {
        result.groups.insert(path, name);
      }
    }

    if (result.decoded > 0 || (loadDisk && disk.size() != result.cache.size())) {
      HashCache::Entries entries;
      entries.reserve(result.cache.size());
      for (auto it = result.cache.cbegin(); it != result.cache.cend(); ++it) {
        entries.insert(it.key(), {it->bytes, it->modified, it->serialize()});
      }
      QList<QString> live;
      live.reserve(work.size());
      for (const Candidate& candidate : work) live.append(candidate.path);
      HashCache::save(cachePath(), kCacheMagic, kCacheVersion, entries, live,
                      kMaximumCacheBytes);
    }
    result.complete = true;
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
