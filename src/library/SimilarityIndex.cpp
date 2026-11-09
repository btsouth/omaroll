#include "library/SimilarityIndex.h"

#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "library/HashCache.h"
#include "sources/CameraRaw.h"
#include "sources/ImageBudget.h"
#include "sources/PinnedImageReader.h"

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
#include <utility>
#include <vector>

namespace {

constexpr int kProgressBatchSize = 16;
constexpr int kSample = 64;
constexpr int kThumbnail = 32;
constexpr int kThumbnailPixels = kThumbnail * kThumbnail;

constexpr quint32 kCacheMagic = 0x4f534931; // OSI1
constexpr quint16 kCacheVersion = 1;
// About 1.1 KB a picture, so this holds roughly 400,000 of them.
constexpr qint64 kMaximumCacheBytes = 512LL * 1024 * 1024;

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

// Grouping limits, so a library of thousands of near-identical pictures stays
// quick and small. Past them a set can come out smaller, never wrong.
constexpr int kMaximumClumpTries = 16;
constexpr std::size_t kMaximumCandidates = 96;
constexpr std::size_t kMaximumBucketScan = 8192;
constexpr qsizetype kMaximumEdges = 1 << 20;
constexpr qsizetype kExactClumpPairs = 9;
// A clump member is within this share of every limit of its first picture.
constexpr double kClumpShare = 0.25;
// Slack for rounding when distances are added up rather than measured.
constexpr double kSlack = 1e-9;

// Each of these is a distance that obeys the triangle inequality, so how far
// apart two members of two clumps can be follows from their first pictures.
// Pearson correlation is the cosine of an angle; word agreement is a Jaccard
// index; the aspect check is a bound on the ratio's logarithm.
double aspectDistance(const SimilarityIndex::Signature& left,
                      const SimilarityIndex::Signature& right) {
  return std::abs(std::log(double(left.width) / left.height) -
                  std::log(double(right.width) / right.height));
}
const double kAspectLimit = -std::log(0.92);

int lumaOf(const SimilarityIndex::Signature& signature) {
  return (signature.red + signature.green + signature.blue) / 3;
}

int tintDistance(const SimilarityIndex::Signature& left, const SimilarityIndex::Signature& right) {
  const int leftLuma = lumaOf(left);
  const int rightLuma = lumaOf(right);
  return std::abs((left.red - leftLuma) - (right.red - rightLuma)) +
         std::abs((left.green - leftLuma) - (right.green - rightLuma)) +
         std::abs((left.blue - leftLuma) - (right.blue - rightLuma));
}

double angleBetween(const SimilarityIndex::Signature& left,
                    const SimilarityIndex::Signature& right) {
  return std::acos(std::clamp(correlation(left, right), -1.0, 1.0));
}

bool flat(const SimilarityIndex::Signature& signature) { return variance(signature) < 4.0; }

bool hasWords(const QStringList& words) { return words.size() >= kMinimumWords; }

// How far a clump's members are from its first picture, at most.
struct Spread {
  double aspect = 0;
  double luma = 0;
  double tint = 0;
  double words = 0;
  double shade = 0;
  double angle = 0;

  void widen(const Spread& other) {
    aspect = std::max(aspect, other.aspect);
    luma = std::max(luma, other.luma);
    tint = std::max(tint, other.tint);
    words = std::max(words, other.words);
    shade = std::max(shade, other.shade);
    angle = std::max(angle, other.angle);
  }
};

const Limits& limitsFor(const SimilarityIndex::Signature& left,
                        const SimilarityIndex::Signature& right, int level) {
  return left.textLike() || right.textLike() ? kTextLike[level] : kPicture[level];
}

// Whether `member` belongs in the clump started by `leader`: the same hash,
// the same kind of picture, and within a share of every limit of it.
bool fitsClump(const SimilarityIndex::Signature& leader, const QStringList& leaderWords,
               const SimilarityIndex::Signature& member, const QStringList& memberWords,
               int level, Spread* distance) {
  if (!member.valid || member.hash != leader.hash || member.textLike() != leader.textLike() ||
      flat(member) != flat(leader) || hasWords(memberWords) != hasWords(leaderWords) ||
      member.width <= 0 || member.height <= 0) {
    return false;
  }
  const Limits& limits = limitsFor(leader, member, level);
  distance->aspect = aspectDistance(leader, member);
  distance->luma = std::abs(lumaOf(leader) - lumaOf(member));
  distance->tint = tintDistance(leader, member);
  if (distance->aspect > kClumpShare * kAspectLimit ||
      distance->luma > kClumpShare * limits.brightness ||
      distance->tint > kClumpShare * limits.tint) {
    return false;
  }
  if (hasWords(leaderWords)) {
    distance->words = 1.0 - wordAgreement(leaderWords, memberWords);
    if (distance->words > kClumpShare * (1.0 - limits.words)) return false;
  }
  if (flat(leader)) {
    distance->shade = std::abs(double(leader.sum - member.sum)) / kThumbnailPixels;
    return distance->shade <= kClumpShare * 6.0;
  }
  distance->angle = angleBetween(leader, member);
  return distance->angle <= kClumpShare * std::acos(limits.correlation);
}

// Whether every member of one clump matches every member of another, judged
// from their first pictures, which match, and each clump's spread. Members
// share their first picture's hash and kind, so only the distances remain.
bool withinSpread(const SimilarityIndex::Signature& left, const QStringList& leftWords,
                  const Spread& leftSpread, const SimilarityIndex::Signature& right,
                  const QStringList& rightWords, const Spread& rightSpread, int level) {
  const Limits& limits = limitsFor(left, right, level);
  if (aspectDistance(left, right) + leftSpread.aspect + rightSpread.aspect >
          kAspectLimit - kSlack ||
      std::abs(lumaOf(left) - lumaOf(right)) + leftSpread.luma + rightSpread.luma >
          limits.brightness ||
      tintDistance(left, right) + leftSpread.tint + rightSpread.tint > limits.tint) {
    return false;
  }
  if (hasWords(leftWords) && hasWords(rightWords) &&
      1.0 - wordAgreement(leftWords, rightWords) + leftSpread.words + rightSpread.words >
          1.0 - limits.words - kSlack) {
    return false;
  }
  if (flat(left)) {
    return std::abs(double(left.sum - right.sum)) / kThumbnailPixels + leftSpread.shade +
               rightSpread.shade <=
           6.0;
  }
  return angleBetween(left, right) + leftSpread.angle + rightSpread.angle <=
         std::acos(limits.correlation) - kSlack;
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
  const int level = std::clamp(strictness, 0, 2);
  const QStringList none;
  const auto wordsAt = [&](int index) -> const QStringList& {
    return index < words.size() ? words.at(index) : none;
  };
  const auto stopped = [cancel] { return cancel && cancel->load(); };

  // First, near-identical pictures with one hash collapse into clumps around
  // their first picture, each within a quarter of every limit of it. Any two
  // members are then within the full limits of each other, so a clump is
  // already a set, and a library of thousands of copies of one screen costs
  // one comparison per picture rather than one per pair.
  struct Clump {
    int leader = 0;
    QList<int> members;
    Spread radius;
  };
  QList<Clump> clumps;
  QHash<quint64, QList<int>> clumpsByHash;
  for (int index = 0; index < signatures.size(); ++index) {
    if (stopped()) return {};
    const Signature& signature = signatures[index];
    if (!signature.valid) continue;
    QList<int>& sameHash = clumpsByHash[signature.hash];
    bool joined = false;
    for (qsizetype k = sameHash.size() - 1, tries = 0; k >= 0 && tries < kMaximumClumpTries;
         --k, ++tries) {
      Clump& clump = clumps[sameHash[k]];
      Spread distance;
      if (fitsClump(signatures[clump.leader], wordsAt(clump.leader), signature, wordsAt(index),
                    level, &distance)) {
        clump.members.append(index);
        clump.radius.widen(distance);
        joined = true;
        break;
      }
    }
    if (!joined) {
      sameHash.append(int(clumps.size()));
      clumps.append({index, {index}, {}});
    }
  }

  // Two clumps link when every picture in one matches every picture in the
  // other: checked in full for small clumps, and otherwise from their first
  // pictures and how far the rest may be from them.
  const auto clumpsMatch = [&](const Clump& a, const Clump& b, double* score) {
    const Signature& left = signatures[a.leader];
    const Signature& right = signatures[b.leader];
    if (!matches(left, right, wordsAt(a.leader), wordsAt(b.leader), level, score)) return false;
    if (a.members.size() == 1 && b.members.size() == 1) return true;
    if (withinSpread(left, wordsAt(a.leader), a.radius, right, wordsAt(b.leader), b.radius,
                     level)) {
      return true;
    }
    if (a.members.size() * b.members.size() > kExactClumpPairs) return false;
    for (int x : a.members) {
      for (int y : b.members) {
        if ((x != a.leader || y != b.leader) &&
            !matches(signatures[x], signatures[y], wordsAt(x), wordsAt(y), level)) {
          return false;
        }
      }
    }
    return true;
  };

  // Candidate pairs: two hashes within the loosest distance at this
  // strictness agree exactly on at least one of that many plus one parts.
  // Buckets hold distinct hashes, and each clump is compared with at most a
  // fixed number of the closest earlier ones, so a library whose hashes are
  // all alike stays linear. Past the limits a set may come out smaller, never
  // wrong.
  const int distance = std::max(kPicture[level].hashDistance, kTextLike[level].hashDistance);
  const int parts = std::max(8, distance + 1);
  std::vector<int> partShift(parts);
  std::vector<int> partBits(parts);
  for (int part = 0, shift = 0; part < parts; ++part) {
    partBits[part] = 64 / parts + (part < 64 % parts ? 1 : 0);
    partShift[part] = shift;
    shift += partBits[part];
  }
  const auto bucketOf = [&](quint64 hash, int part) {
    return part * 256 + int((hash >> partShift[part]) & ((quint64(1) << partBits[part]) - 1));
  };
  std::vector<std::vector<int>> buckets(std::size_t(parts) * 256);
  QHash<quint64, int> hashIds;
  std::vector<quint64> hashValues;
  std::vector<std::vector<int>> clumpsOfHash;
  std::vector<int> seen;

  struct Edge {
    int left;
    int right;
    double score;
  };
  QList<Edge> edges;
  QSet<quint64> linked;
  std::vector<std::pair<int, int>> near;
  std::vector<int> candidates;
  for (int node = 0; node < clumps.size() && edges.size() < kMaximumEdges; ++node) {
    if (stopped()) return {};
    const quint64 hash = signatures[clumps[node].leader].hash;
    near.clear();
    for (int part = 0; part < parts; ++part) {
      const std::vector<int>& bucket = buckets[std::size_t(bucketOf(hash, part))];
      const std::size_t first =
          bucket.size() > kMaximumBucketScan ? bucket.size() - kMaximumBucketScan : 0;
      for (std::size_t k = bucket.size(); k-- > first;) {
        const int id = bucket[k];
        if (seen[std::size_t(id)] == node) continue;
        seen[std::size_t(id)] = node;
        const int bits = std::popcount(hashValues[std::size_t(id)] ^ hash);
        if (bits <= distance) near.emplace_back(bits, -id);
      }
    }
    // Closest hashes first, and of those the most recent.
    std::sort(near.begin(), near.end());
    candidates.clear();
    for (const auto& [bits, negativeId] : near) {
      const std::vector<int>& others = clumpsOfHash[std::size_t(-negativeId)];
      for (std::size_t k = others.size(); k-- > 0 && candidates.size() < kMaximumCandidates;) {
        candidates.push_back(others[k]);
      }
      if (candidates.size() >= kMaximumCandidates) break;
    }
    for (int other : candidates) {
      if (stopped()) return {};
      double score = 0;
      if (edges.size() < kMaximumEdges && clumpsMatch(clumps[other], clumps[node], &score)) {
        edges.append({other, node, score});
        linked.insert(pairKey(other, node));
      }
    }
    auto id = hashIds.constFind(hash);
    if (id == hashIds.cend()) {
      id = hashIds.insert(hash, int(hashValues.size()));
      hashValues.push_back(hash);
      clumpsOfHash.emplace_back();
      seen.push_back(-1);
      for (int part = 0; part < parts; ++part) {
        buckets[std::size_t(bucketOf(hash, part))].push_back(id.value());
      }
    }
    clumpsOfHash[std::size_t(id.value())].push_back(node);
  }

  // Complete linkage, closest pairs first: two sets merge only when every
  // clump in one links with every clump in the other.
  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.left != b.left ? a.left < b.left : a.right < b.right;
  });
  QList<int> sets(clumps.size());
  std::vector<QList<int>> members(std::size_t(clumps.size()));
  for (int node = 0; node < sets.size(); ++node) {
    sets[node] = node;
    members[std::size_t(node)] = {node};
  }
  for (const Edge& edge : std::as_const(edges)) {
    if (stopped()) return {};
    const int left = findSet(sets, edge.left);
    const int right = findSet(sets, edge.right);
    if (left == right) continue;
    const QList<int>& leftMembers = members[std::size_t(left)];
    const QList<int>& rightMembers = members[std::size_t(right)];
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
    members[std::size_t(left)].append(rightMembers);
    members[std::size_t(right)] = {};
  }
  QList<QList<int>> result;
  for (const QList<int>& nodes : members) {
    QList<int> set;
    for (int node : nodes) set.append(clumps[node].members);
    if (set.size() < 2) continue;
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
    PinnedImageReader reader(path);
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
  m_textTimer.setSingleShot(true);
  m_textTimer.setInterval(1500);
  connect(&m_textTimer, &QTimer::timeout, this, [this] {
    if (m_active && m_textChanged && !m_watcher.isRunning()) start();
  });
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
    } else if (m_active && m_textChanged) {
      m_textTimer.start();
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
  // A running scan finishes with the text it started with, then one more
  // runs; cancelling it on every result could keep it from ever finishing.
  if (m_active && m_groups.contains(path)) {
    m_textChanged = true;
    if (!m_watcher.isRunning()) m_textTimer.start();
  }
}

void SimilarityIndex::requestStop() {
  m_refreshTimer.stop();
  m_textTimer.stop();
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
    m_textTimer.stop();
    m_textChanged = false;
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
  // The word lists are copied below, so this scan sees every result so far.
  m_textChanged = false;
  m_textTimer.stop();
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
