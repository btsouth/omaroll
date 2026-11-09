#include "thumbs/Storyboard.h"

#include "app/TsanHandoff.h"
#include "sources/FileVersion.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// ---------------------------------------------------------------------------
// Plan

QList<qint64> StoryboardPlan::sampleTimes(qint64 durationMs) {
  if (durationMs < kMinimumDurationMs) {
    return {};
  }
  // Equal slots that fit the clip exactly, each at least the minimum long.
  const qint64 count = std::clamp<qint64>(durationMs / kMinimumIntervalMs, 1, kMaximumFrames);
  QList<qint64> times;
  times.reserve(count);
  for (qint64 slot = 0; slot < count; ++slot) {
    times.append((2 * slot + 1) * durationMs / (2 * count));
  }
  return times;
}

QSize StoryboardPlan::pixelBox(qreal devicePixelRatio) {
  const int scale = devicePixelRatio > 1.0 ? 2 : 1;
  return {kBoxWidth * scale, kBoxHeight * scale};
}

QList<int> StoryboardPlan::extractionOrder(int count) {
  QList<int> order;
  if (count <= 0) {
    return order;
  }
  order.reserve(count);
  std::vector<bool> taken(size_t(count), false);
  int step = 1;
  while (step < count) {
    step *= 2;
  }
  for (; step >= 1; step /= 2) {
    for (int index = 0; index < count; index += step) {
      if (!taken[size_t(index)]) {
        taken[size_t(index)] = true;
        order.append(index);
      }
    }
  }
  return order;
}

int StoryboardPlan::nearest(const QList<qint64>& sortedTimes, qint64 at) {
  if (sortedTimes.isEmpty()) {
    return -1;
  }
  const auto after = std::lower_bound(sortedTimes.cbegin(), sortedTimes.cend(), at);
  if (after == sortedTimes.cbegin()) {
    return 0;
  }
  if (after == sortedTimes.cend()) {
    return int(sortedTimes.size()) - 1;
  }
  const auto before = std::prev(after);
  return int((at - *before <= *after - at ? before : after) - sortedTimes.cbegin());
}

qint64 StoryboardPlan::tolerance(const QList<qint64>& times) {
  // The first target is the middle of the first slot: half a slot in.
  return times.isEmpty() ? 0 : times.constFirst() / 2;
}

bool StoryboardFrames::insert(qint64 time, const QByteArray& jpeg) {
  if (jpeg.isEmpty()) {
    return false;
  }
  const auto at = std::lower_bound(times.begin(), times.end(), time);
  if (at != times.end() && *at == time) {
    return false;
  }
  if (!frameSize.isValid()) {
    QBuffer buffer;
    buffer.setData(jpeg);
    frameSize = QImageReader(&buffer).size();
  }
  const qsizetype index = at - times.begin();
  times.insert(index, time);
  jpegs.insert(index, jpeg);
  return true;
}

void StoryboardFrames::markSampled(qint64 target) {
  const auto at = std::lower_bound(sampled.begin(), sampled.end(), target);
  if (at == sampled.end() || *at != target) {
    sampled.insert(at - sampled.begin(), target);
  }
}

bool StoryboardFrames::wasSampled(qint64 target, qint64 tolerance) const {
  const auto at = std::lower_bound(sampled.cbegin(), sampled.cend(), target - tolerance);
  return at != sampled.cend() && *at <= target + tolerance;
}

bool StoryboardFrames::covers(const QList<qint64>& times, qint64 tolerance) const {
  return !times.isEmpty() && std::all_of(times.cbegin(), times.cend(), [&](qint64 target) {
    return wasSampled(target, tolerance);
  });
}

void StoryboardFrames::keepSampled(const QList<qint64>& times, qint64 tolerance) {
  sampled.removeIf([&](qint64 target) {
    const int index = StoryboardPlan::nearest(times, target);
    return index < 0 || qAbs(times.at(index) - target) > tolerance;
  });
}

bool StoryboardFrames::merge(const StoryboardFrames& other) {
  bool added = false;
  for (qsizetype index = 0; index < other.times.size(); ++index) {
    added = insert(other.times.at(index), other.jpegs.at(index)) || added;
  }
  for (const qint64 target : other.sampled) {
    markSampled(target);
  }
  complete = complete || other.complete;
  return added;
}

// ---------------------------------------------------------------------------
// Disk cache

namespace {

constexpr quint32 kMagic = 0x4f4d5342; // "OMSB"
// 2 added the sampled times and the complete flag. 3 reads frame times
// written in scientific notation, which 2 could place a second out.
constexpr quint16 kFormat = 3;
constexpr int kMaximumStoredFrames = 4 * StoryboardPlan::kMaximumFrames;
constexpr qsizetype kMaximumFrameBytes = 2 * 1024 * 1024;
const QString kSuffix = QStringLiteral(".storyboard");

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured;
}

QString entryPath(const QString& key) {
  return StoryboardCache::directory() + QLatin1Char('/') + key + kSuffix;
}

constexpr QFileDevice::Permissions kOwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

// A frame worth serving: whole JPEG data that decodes.
bool decodes(const QByteArray& jpeg) {
  if (!jpeg.startsWith("\xff\xd8") || !jpeg.endsWith("\xff\xd9")) {
    return false;
  }
  QBuffer buffer;
  buffer.setData(jpeg);
  QImageReader reader(&buffer, "jpeg");
  QImage image;
  return reader.read(&image) && !image.isNull();
}

} // namespace

QString StoryboardCache::directory() {
  return cacheHome() + QStringLiteral("/omaroll/storyboards");
}

QString StoryboardCache::key(const QString& path, const QSize& pixelBox) {
  const QString version = FileVersion::diskKey(path);
  if (path.isEmpty() || version.isEmpty()) {
    return {};
  }
  // One multi-argument arg(), so a "%2" in the path is never substituted.
  const QString identity = QStringLiteral("storyboard%1|%2|%3|%4x%5")
                               .arg(QString::number(kFormat), path, version,
                                    QString::number(pixelBox.width()),
                                    QString::number(pixelBox.height()));
  return QString::fromLatin1(
      QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Md5).toHex());
}

std::optional<StoryboardFrames> StoryboardCache::load(const QString& key) {
  if (key.isEmpty()) {
    return std::nullopt;
  }
  const QString path = entryPath(key);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  // An entry written before the cache was owner-only becomes so.
  struct stat info {};
  if (::fstat(file.handle(), &info) == 0 && (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    ::fchmod(file.handle(), S_IRUSR | S_IWUSR);
  }
  QDataStream in(&file);
  in.setVersion(QDataStream::Qt_6_5);
  quint32 magic = 0;
  quint16 format = 0;
  StoryboardFrames frames;
  qint32 count = 0;
  in >> magic >> format >> frames.frameSize >> count;
  if (in.status() != QDataStream::Ok || magic != kMagic || format != kFormat ||
      !frames.frameSize.isValid() || frames.frameSize.isEmpty() || count <= 0 ||
      count > kMaximumStoredFrames) {
    return std::nullopt;
  }
  frames.times.reserve(count);
  frames.jpegs.reserve(count);
  for (qint32 index = 0; index < count; ++index) {
    // The frame's length is checked before anything is allocated for it.
    // It is a QByteArray as QDataStream writes one this small: a 32-bit
    // length, then the bytes.
    qint64 time = 0;
    quint32 length = 0;
    in >> time >> length;
    if (in.status() != QDataStream::Ok || length == 0 || length > quint32(kMaximumFrameBytes) ||
        (!frames.times.isEmpty() && time <= frames.times.constLast()) || time < 0) {
      return std::nullopt;
    }
    QByteArray jpeg(qsizetype(length), Qt::Uninitialized);
    if (in.readRawData(jpeg.data(), int(length)) != int(length) || !decodes(jpeg)) {
      return std::nullopt;
    }
    frames.times.append(time);
    frames.jpegs.append(jpeg);
  }
  qint32 sampledCount = 0;
  in >> sampledCount >> frames.complete;
  if (in.status() != QDataStream::Ok || sampledCount < 0 || sampledCount > kMaximumStoredFrames) {
    return std::nullopt;
  }
  frames.sampled.reserve(sampledCount);
  for (qint32 index = 0; index < sampledCount; ++index) {
    qint64 target = 0;
    in >> target;
    if (in.status() != QDataStream::Ok || target < 0 ||
        (!frames.sampled.isEmpty() && target <= frames.sampled.constLast())) {
      return std::nullopt;
    }
    frames.sampled.append(target);
  }
  file.close();
  // Recently used, for eviction. Relatime makes the access time unreliable.
  ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), nullptr, 0);
  return frames;
}

#ifdef OMAROLL_FUZZ
void StoryboardCache::fuzzLoad(const QByteArray& bytes) {
  static const QString fuzzKey = QStringLiteral("fuzz");
  if (!QDir().mkpath(directory())) std::abort();
  QFile file(entryPath(fuzzKey));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
    std::abort();
  file.close();
  const std::optional<StoryboardFrames> frames = load(fuzzKey);
  if (!frames) return;
  // A loaded entry is what the viewer trusts: sorted, bounded and paired.
  if (frames->times.size() != frames->jpegs.size() || frames->times.isEmpty() ||
      !std::is_sorted(frames->times.cbegin(), frames->times.cend()) ||
      !std::is_sorted(frames->sampled.cbegin(), frames->sampled.cend()))
    std::abort();
  if (StoryboardPlan::nearest(frames->times, frames->times.constLast() / 2) < 0) std::abort();
}
#endif

bool StoryboardCache::save(const QString& key, const StoryboardFrames& frames) {
  if (key.isEmpty() || frames.isEmpty() || !frames.frameSize.isValid()) {
    return false;
  }
  const QString path = entryPath(key);
  const QString folder = directory();
  if (!QDir().mkpath(folder) ||
      !QFile::setPermissions(folder, kOwnerOnly | QFileDevice::ExeOwner)) {
    return false;
  }
  // Written beside and renamed into place, so a reader sees a whole file or
  // none. Owner-only from the start, whatever the umask.
  const QString staging = path + QLatin1Char('.') +
                          QString::number(QCoreApplication::applicationPid()) + QLatin1Char('.') +
                          QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId())) +
                          QStringLiteral(".tmp");
  const QByteArray stagingName = QFile::encodeName(staging);
  const int descriptor = ::open(stagingName.constData(),
                                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    return false;
  }
  // One left over from a process that had this pid, created more openly.
  ::fchmod(descriptor, S_IRUSR | S_IWUSR);
  QFile file;
  if (!file.open(descriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
    ::close(descriptor);
    ::unlink(stagingName.constData());
    return false;
  }
  QDataStream out(&file);
  out.setVersion(QDataStream::Qt_6_5);
  out << kMagic << kFormat << frames.frameSize << qint32(frames.times.size());
  for (qsizetype index = 0; index < frames.times.size(); ++index) {
    out << frames.times.at(index) << frames.jpegs.at(index);
  }
  out << qint32(frames.sampled.size()) << frames.complete;
  for (const qint64 target : frames.sampled) {
    out << target;
  }
  const bool written = out.status() == QDataStream::Ok && file.flush();
  file.close();
  if (!written || ::rename(stagingName.constData(), QFile::encodeName(path).constData()) != 0) {
    ::unlink(stagingName.constData());
    return false;
  }
  return true;
}

void StoryboardCache::prune(qint64 maximumBytes) {
  QDir dir(directory());
  if (!dir.exists()) {
    return;
  }
  QFileInfoList entries = dir.entryInfoList({QStringLiteral("*") + kSuffix}, QDir::Files);
  qint64 total = 0;
  for (const QFileInfo& entry : entries) {
    total += entry.size();
  }
  // A staging file is "<entry>.<pid>.<thread>.tmp". One still being written
  // is young and counts toward the limit; one left by a process that died
  // while saving is old, and goes.
  const QDateTime abandoned = QDateTime::currentDateTime().addMSecs(-kAbandonedStagingMs);
  for (const QFileInfo& staging :
       dir.entryInfoList({QStringLiteral("*") + kSuffix + QStringLiteral(".*.tmp")}, QDir::Files)) {
    if (staging.lastModified() >= abandoned || !QFile::remove(staging.absoluteFilePath())) {
      total += staging.size();
    }
  }
  if (total <= maximumBytes) {
    return;
  }
  std::sort(entries.begin(), entries.end(), [](const QFileInfo& a, const QFileInfo& b) {
    return a.lastModified() < b.lastModified();
  });
  for (const QFileInfo& entry : entries) {
    if (total <= maximumBytes) {
      break;
    }
    const qint64 size = entry.size();
    if (QFile::remove(entry.absoluteFilePath())) {
      total -= size;
    }
  }
}

// ---------------------------------------------------------------------------
// Extraction

namespace {

std::atomic_bool g_stopping{false};
std::atomic_int g_running{0};
std::atomic_int g_launched{0};
std::atomic<quint64> g_instances{0};
std::atomic<quint64> g_jobs{0};

// Files ffmpeg turned down: no video, or none it can read. Left alone until
// they change, or for a while, since a mount coming back or a codec being
// installed can change the answer without changing the file.
QMutex g_failedMutex;
QHash<QString, QDeadlineTimer> g_failed;
std::atomic_int g_failureMemoryMs{10 * 60 * 1000};

bool knownFailure(const QString& key) {
  QMutexLocker lock(&g_failedMutex);
  const auto found = g_failed.constFind(key);
  if (found == g_failed.cend()) {
    return false;
  }
  if (found->hasExpired()) {
    g_failed.erase(found);
    return false;
  }
  return true;
}

void rememberFailure(const QString& key) {
  QMutexLocker lock(&g_failedMutex);
  g_failed.insert(key, QDeadlineTimer(g_failureMemoryMs.load()));
}

// One storyboard decodes at a time; cache reads have their own lane so a
// cached video opens with previews while another is still extracting. Of the
// storyboards waiting for the decoder, the newest goes first. A window that
// loses focus cancels its own, so the one in front never waits behind it.
//
// Never destroyed: a worker blocked on a stalled mount would otherwise hold
// up exit in the pool's destructor. Shutdown waits a moment, then leaves it.
struct Pools {
  QThreadPool cache;
  QThreadPool extraction;
};

// Both made on first use, together, so they belong to the GUI thread.
Pools& pools() {
  static Pools* const created = [] {
    auto* both = new Pools;
    both->cache.setMaxThreadCount(2);
    both->extraction.setMaxThreadCount(1);
    return both;
  }();
  return *created;
}

QThreadPool& extractionPool() { return pools().extraction; }
QThreadPool& cachePool() { return pools().cache; }

// How long exit waits for the workers to save what they decoded.
constexpr int kShutdownWaitMs = 1500;

// Frames for the image provider, by storyboard token.
QMutex g_registryMutex;
QHash<QString, std::shared_ptr<const StoryboardFrames>> g_registry;

// The helper deadline the thumbnail cache uses for one frame. A sample that
// misses it is tried again the next time the video opens.
std::atomic_int g_frameTimeoutMs{8000};
constexpr int kPollMs = 20;
// The first few samples all failing means there is no usable video.
constexpr int kGiveUpAttempts = 3;

QStringList frameArguments(const QString& path, qint64 at, const QSize& box) {
  // Keyframes only: -skip_frame drops everything else before decoding, and
  // -noaccurate_seek returns the keyframe at or before the target rather than
  // decoding forward to it. That keeps one sample to one decoded picture,
  // even with a ten-second GOP. showinfo reports the frame's timestamp
  // relative to the target, which gives the frame's real time.
  // Square pixels first, then down to the box, never up.
  const QString scale =
      QStringLiteral("scale='trunc(iw*sar/2)*2':ih,setsar=1,"
                     "scale='min(%1,iw)':'min(%2,ih)':force_original_aspect_ratio=decrease:"
                     "force_divisible_by=2:flags=area,showinfo")
          .arg(box.width())
          .arg(box.height());
  return {
      QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-nostats"),
      QStringLiteral("-loglevel"), QStringLiteral("info"),
      // One frame needs no decoder threads, and a single thread costs less.
      QStringLiteral("-threads"), QStringLiteral("1"),
      QStringLiteral("-skip_frame"), QStringLiteral("nokey"),
      QStringLiteral("-ss"), QString::number(double(at) / 1000.0, 'f', 3),
      QStringLiteral("-noaccurate_seek"),
      QStringLiteral("-i"), path,
      // The first playable video stream: "V" leaves out cover art and other
      // attached pictures, which may come first.
      QStringLiteral("-map"), QStringLiteral("0:V:0"),
      QStringLiteral("-an"), QStringLiteral("-sn"), QStringLiteral("-dn"),
      QStringLiteral("-frames:v"), QStringLiteral("1"),
      QStringLiteral("-vf"), scale,
      QStringLiteral("-filter_threads"), QStringLiteral("1"),
      QStringLiteral("-fps_mode"), QStringLiteral("passthrough"),
      QStringLiteral("-f"), QStringLiteral("image2pipe"),
      QStringLiteral("-c:v"), QStringLiteral("mjpeg"),
      QStringLiteral("-q:v"), QStringLiteral("5"),
      QStringLiteral("pipe:1"),
  };
}

qint64 frameTimeFrom(const QByteArray& log, qint64 target) {
  // showinfo's line for the first frame through the filter. Only that line:
  // the input's path, and its metadata, are in the log too and may contain
  // anything, "pts_time:" included.
  static const QRegularExpression pts(
      QStringLiteral("^\\[Parsed_showinfo_[0-9]+ @ 0x[0-9a-fA-F]+\\] n:\\s*0 .*?"
                     "\\bpts_time:\\s*(-?[0-9]+(?:\\.[0-9]*)?(?:[eE][-+]?[0-9]+)?)"),
      QRegularExpression::MultilineOption);
  const QRegularExpressionMatch match = pts.match(QString::fromUtf8(log));
  if (!match.hasMatch()) {
    return target;
  }
  bool okay = false;
  const double offset = match.captured(1).toDouble(&okay);
  return okay ? std::max<qint64>(0, target + qint64(std::llround(offset * 1000.0))) : target;
}

} // namespace

struct StoryboardJob {
  // The source as the owner has it. The absolute path and the key are worked
  // out on the cache lane, since both may touch a slow mount.
  QString source;
  QString path;
  QString key;
  QSize box;
  QList<qint64> times;
  qint64 tolerance = 0;
  // What the owner already has, from an earlier run that was cancelled or
  // cut short, then whatever the cache adds. Only used if `knownKey` is
  // still the file's key.
  StoryboardFrames known;
  QString knownKey;
  quint64 generation = 0;
  quint64 serial = 0;
  std::atomic_bool cancelled{false};
  std::atomic_bool paused{false};

  [[nodiscard]] bool isCancelled() const { return cancelled.load() || g_stopping.load(); }

  // The owner is cleared under the lock before it goes, so a post either
  // reaches a live owner's queue or is dropped. The queued call is tied to
  // the owner as its context, so Qt discards it if the owner dies first.
  template <typename Function> void post(Function function) {
    QMutexLocker lock(&mutex);
    if (Storyboard* target = owner) {
      QMetaObject::invokeMethod(
          target, [target, function = std::move(function)] { function(target); },
          Qt::QueuedConnection);
    }
  }

  void attach(Storyboard* storyboard) {
    QMutexLocker lock(&mutex);
    owner = storyboard;
  }

  void detach() {
    QMutexLocker lock(&mutex);
    owner = nullptr;
    cancelled.store(true);
  }

  // A sample tried to the end: its frame, or no jpeg when ffmpeg found
  // nothing there.
  void postSample(qint64 target, qint64 time, const QByteArray& jpeg) {
    tsan::handOver(jpeg.constData());
    post([generation = generation, target, time, jpeg](Storyboard* owner_) {
      tsan::takeOver(jpeg.constData());
      owner_->acceptSample(generation, target, time, jpeg);
    });
  }

  void postKey() {
    post([serial = serial, key = key](Storyboard* owner_) { owner_->adoptKey(serial, key); });
  }

  void postCached(const StoryboardFrames& frames, bool complete) {
    post([serial = serial, frames, complete](Storyboard* owner_) {
      owner_->acceptCached(serial, frames, complete);
    });
  }

  void postFinished(Storyboard::Outcome outcome, bool complete) {
    post([serial = serial, outcome, complete](Storyboard* owner_) {
      owner_->finish(serial, outcome, complete);
    });
  }

  // Folds the cache entry, if any, into `known` and tells the owner. True
  // when that covers this plan, which leaves nothing to decode. An entry
  // saved for another duration's plan is used for what it has.
  bool absorbCache() {
    std::optional<StoryboardFrames> cached = StoryboardCache::load(key);
    if (!cached) {
      return false;
    }
    const bool added = known.merge(*cached);
    const bool complete = known.covers(times, tolerance);
    if (added || complete) {
      postCached(*cached, complete);
    }
    return complete;
  }

  // The file is still the version this run started on.
  [[nodiscard]] bool unchanged() const { return StoryboardCache::key(path, box) == key; }

  void extract();

private:
  QMutex mutex;
  Storyboard* owner = nullptr;
};

void StoryboardJob::extract() {
  if (isCancelled()) {
    return;
  }
  // Another window may have filled the cache while this one waited.
  if (absorbCache()) {
    return;
  }
  const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (ffmpeg.isEmpty() || times.isEmpty()) {
    postFinished(Storyboard::Outcome::Retryable, false);
    return;
  }

  struct Running {
    std::unique_ptr<QProcess> process;
    qint64 target = 0;
    QElapsedTimer elapsed;
  };
  std::vector<Running> running;
  StoryboardFrames frames = known;
  // Coarse to fine, without the samples an earlier run already tried.
  QList<qint64> pending;
  for (const int index : StoryboardPlan::extractionOrder(int(times.size()))) {
    if (!frames.wasSampled(times.at(index), tolerance)) {
      pending.append(times.at(index));
    }
  }
  qsizetype next = 0;
  int attempts = 0;
  // Attempts ffmpeg itself turned down, exiting with an error. Only a run
  // where every attempt was one of these says the file has no usable video;
  // a timeout, a crash or a failure to start says nothing about the file.
  int declined = 0;
  int tried = 0;
  bool changed = false;
  const int frameTimeoutMs = g_frameTimeoutMs.load();

  const auto stop = [](Running& item) {
    if (item.process->state() != QProcess::NotRunning) {
      item.process->kill();
      item.process->waitForFinished(1000);
    }
    g_running.fetch_sub(1);
  };

  while (!isCancelled() && !changed) {
    const bool givenUp = attempts >= kGiveUpAttempts && frames.isEmpty();
    while (!givenUp && !paused.load() && running.size() < size_t(Storyboard::kParallelProcesses) &&
           next < pending.size()) {
      Running item;
      item.target = pending.at(next++);
      item.process = std::make_unique<QProcess>();
      // Out of the way of playback and the desktop: the lowest CPU priority
      // and the lowest best-effort disk priority (level 7), which still makes
      // progress on a busy machine where the idle class could starve it.
      item.process->setChildProcessModifier([] {
        ::setpriority(PRIO_PROCESS, 0, 19);
        constexpr int kBestEffort = 2;
        constexpr int kLowest = 7;
        ::syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, (kBestEffort << 13) | kLowest);
      });
      item.process->start(ffmpeg, frameArguments(path, item.target, box));
      item.elapsed.start();
      g_running.fetch_add(1);
      g_launched.fetch_add(1);
      running.push_back(std::move(item));
    }
    if (running.empty()) {
      if (givenUp || next >= pending.size()) {
        break;
      }
      QThread::msleep(kPollMs); // paused
      continue;
    }

    for (auto item = running.begin(); item != running.end();) {
      QProcess& process = *item->process;
      if (process.state() != QProcess::NotRunning) {
        process.waitForFinished(kPollMs / int(running.size()));
      }
      if (process.state() == QProcess::NotRunning) {
        ++attempts;
        const QByteArray jpeg = process.readAllStandardOutput();
        const bool started = process.error() != QProcess::FailedToStart;
        const bool exited = started && process.exitStatus() == QProcess::NormalExit;
        const bool okay = exited && process.exitCode() == 0 && jpeg.startsWith("\xff\xd8");
        // Exited cleanly with nothing to show: there is no frame there, and
        // asking again would find none. An error exit, a crash or a failure
        // to start is left untried, so the next run asks again.
        const bool final = okay || (exited && process.exitCode() == 0);
        if (exited && process.exitCode() != 0) {
          ++declined;
        }
        // A frame decoded from a file that has since changed may be from
        // either version, so this run stops and the owner starts over.
        if (!isCancelled() && !unchanged()) {
          changed = true;
        } else if (!isCancelled() && final) {
          const qint64 time = okay ? frameTimeFrom(process.readAllStandardError(), item->target) : -1;
          if (okay) {
            frames.insert(time, jpeg);
          }
          frames.markSampled(item->target);
          ++tried;
          postSample(item->target, time, okay ? jpeg : QByteArray());
        }
        g_running.fetch_sub(1);
        item = running.erase(item);
      } else if (item->elapsed.elapsed() > frameTimeoutMs) {
        // Left out of `sampled`, so the next run tries it again.
        ++attempts;
        stop(*item);
        item = running.erase(item);
      } else {
        ++item;
      }
    }
  }

  for (Running& item : running) {
    stop(item);
  }
  if (changed || (tried > 0 && !unchanged())) {
    // Nothing is saved under the old version's key that the new one wrote.
    if (!isCancelled()) {
      postFinished(Storyboard::Outcome::Changed, false);
    }
    return;
  }
  // Samples from another duration's plan stand in for this one's where they
  // are close enough; the rest are not worth keeping.
  frames.keepSampled(times, tolerance);
  frames.complete = frames.covers(times, tolerance);
  // What was decoded is kept, cancelled or not, so switching away and back
  // never decodes a sample twice.
  if (tried > 0 && !frames.isEmpty()) {
    StoryboardCache::save(key, frames);
    StoryboardCache::prune();
  }
  if (isCancelled()) {
    return;
  }
  if (frames.isEmpty()) {
    // A file ffmpeg turned down every time is left alone for a while. One
    // that was only slow, or a decoder that crashed or could not start, is
    // tried again the next time the video is shown.
    if (attempts > 0 && declined == attempts) {
      rememberFailure(key);
      postFinished(Storyboard::Outcome::Failed, false);
    } else {
      postFinished(Storyboard::Outcome::Retryable, false);
    }
    return;
  }
  postFinished(Storyboard::Outcome::Decoded, frames.complete);
}

// ---------------------------------------------------------------------------
// Image provider

namespace {

// Ids are "<token>/<frame time>". Decoding a frame this size takes well under
// a millisecond, so it stays synchronous and the preview never flickers.
class StoryboardProvider final : public QQuickImageProvider {
public:
  StoryboardProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
    const qsizetype slash = id.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) {
      return {};
    }
    bool okay = false;
    const qint64 time = QStringView{id}.mid(slash + 1).toLongLong(&okay);
    if (!okay) {
      return {};
    }
    std::shared_ptr<const StoryboardFrames> frames;
    {
      QMutexLocker lock(&g_registryMutex);
      frames = g_registry.value(id.left(slash));
    }
    if (!frames || frames->isEmpty()) {
      return {};
    }
    const int index = StoryboardPlan::nearest(frames->times, time);
    QImage image = QImage::fromData(frames->jpegs.at(index), "JPG");
    if (!image.isNull() && requestedSize.isValid() && !requestedSize.isEmpty() &&
        (image.width() > requestedSize.width() || image.height() > requestedSize.height())) {
      image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size) {
      *size = image.size();
    }
    return image;
  }
};

} // namespace

// ---------------------------------------------------------------------------
// QML object

Storyboard::Storyboard(QObject* parent) : QObject(parent), m_instance(++g_instances) {
  m_token = QStringLiteral("%1-0").arg(m_instance);
  m_startTimer.setSingleShot(true);
  m_startTimer.setInterval(kStartDelayMs);
  connect(&m_startTimer, &QTimer::timeout, this, &Storyboard::start);
}

Storyboard::~Storyboard() {
  cancel();
  QMutexLocker lock(&g_registryMutex);
  g_registry.remove(m_token);
}

void Storyboard::install(QQmlEngine& engine) {
  static const bool registered = [] {
    qmlRegisterType<Storyboard>("Omaroll.Media", 1, 0, "Storyboard");
    return true;
  }();
  Q_UNUSED(registered);
  engine.addImageProvider(QLatin1String(kProviderId), new StoryboardProvider);
}

void Storyboard::shutdown() {
  g_stopping.store(true);
  // One deadline for both: the cache lane first, since it hands work on to
  // the decoder lane.
  const QDeadlineTimer deadline(kShutdownWaitMs);
  cachePool().waitForDone(deadline);
  extractionPool().waitForDone(deadline);
}

void Storyboard::waitForWorkers() {
  cachePool().waitForDone();
  extractionPool().waitForDone();
}

int Storyboard::runningProcesses() { return g_running.load(); }
int Storyboard::launchedProcesses() { return g_launched.load(); }

#ifdef OMAROLL_TESTING
void Storyboard::setFrameTimeoutMs(int milliseconds) { g_frameTimeoutMs.store(milliseconds); }
void Storyboard::setFailureMemoryMs(int milliseconds) { g_failureMemoryMs.store(milliseconds); }
#endif

void Storyboard::setSource(const QString& source) {
  if (m_source == source) return;
  m_source = source;
  emit sourceChanged();
  reconsider();
}

void Storyboard::setVersion(const QString& version) {
  if (m_version == version) return;
  m_version = version;
  emit versionChanged();
  reconsider();
}

void Storyboard::setDuration(qreal duration) {
  if (m_duration == duration) return;
  m_duration = duration;
  emit durationChanged();
  reconsider();
}

void Storyboard::setDevicePixelRatio(qreal ratio) {
  if (m_devicePixelRatio == ratio) return;
  m_devicePixelRatio = ratio;
  emit devicePixelRatioChanged();
  reconsider();
}

void Storyboard::setActive(bool active) {
  if (m_active == active) return;
  m_active = active;
  emit activeChanged();
  reconsider();
}

void Storyboard::setPaused(bool paused) {
  if (m_paused == paused) return;
  m_paused = paused;
  if (m_job) m_job->paused.store(paused);
  emit pausedChanged();
}

qreal Storyboard::aspectRatio() const {
  const QSize size = m_frames.frameSize;
  return size.isValid() && size.height() > 0 ? qreal(size.width()) / size.height() : 16.0 / 9.0;
}

int Storyboard::frameAt(qreal milliseconds) const {
  return StoryboardPlan::nearest(m_frames.times, qint64(std::llround(milliseconds)));
}

qreal Storyboard::frameTime(int index) const {
  return index >= 0 && index < m_frames.times.size() ? qreal(m_frames.times.at(index)) : -1;
}

QString Storyboard::frameUrl(int index) const {
  if (index < 0 || index >= m_frames.times.size()) {
    return {};
  }
  return QStringLiteral("image://%1/%2/%3")
      .arg(QLatin1String(kProviderId), m_token, QString::number(m_frames.times.at(index)));
}

void Storyboard::reconsider() {
  const QSize box = StoryboardPlan::pixelBox(m_devicePixelRatio);
  const bool eligible = !m_source.isEmpty() &&
                        m_duration >= qreal(StoryboardPlan::kMinimumDurationMs);
  // Concatenated, not arg(), so a "%1" in the path stays in the path.
  const QString identity = eligible ? m_source + QLatin1Char('|') + m_version + QLatin1Char('|') +
                                          QString::number(box.width()) + QLatin1Char('x') +
                                          QString::number(box.height())
                                    : QString();
  bool changed = false;
  if (identity != m_identity) {
    changed = cancel();
    clear();
    m_identity = identity;
    m_restartedForChange = false;
  }
  // A new duration is a new plan. A run for the old one stops, keeping what
  // it decoded, and the next fills in the new targets; frames already
  // decoded stay on show meanwhile.
  const QList<qint64> plan =
      identity.isEmpty() ? QList<qint64>{} : StoryboardPlan::sampleTimes(qint64(m_duration));
  if (plan != m_plan) {
    m_plan = plan;
    changed = cancel() || changed;
    const bool complete = m_frames.covers(m_plan, StoryboardPlan::tolerance(m_plan));
    if (complete != m_complete) {
      m_complete = complete;
      changed = true;
    }
  }
  if (identity.isEmpty() || m_complete || m_failed) {
    m_startTimer.stop();
  } else if (!m_active) {
    m_startTimer.stop();
    // Whatever was decoded so far stays on show, and the job saves it, but no
    // decoder runs for a viewer that is closed, behind another window or
    // showing something else.
    changed = cancel() || changed;
  } else if (!m_job && !m_startTimer.isActive()) {
    m_startTimer.start();
  }
  if (changed) {
    emit stateChanged();
  }
}

void Storyboard::start() {
  if (m_job || m_identity.isEmpty() || !m_active || m_complete || m_failed) {
    return;
  }
  if (m_plan.isEmpty()) {
    m_failed = true;
    emit stateChanged();
    return;
  }
  auto job = std::make_shared<StoryboardJob>();
  job->source = m_source;
  job->box = StoryboardPlan::pixelBox(m_devicePixelRatio);
  job->times = m_plan;
  job->tolerance = StoryboardPlan::tolerance(m_plan);
  job->known = m_frames;
  job->knownKey = m_framesKey;
  job->generation = m_generation;
  job->serial = ++g_jobs;
  job->paused.store(m_paused);
  job->attach(this);
  m_job = job;
  m_fromCache = false;
  emit stateChanged();

  cachePool().start([job] {
    if (job->isCancelled()) {
      return;
    }
    job->path = QFileInfo(job->source).absoluteFilePath();
    job->key = StoryboardCache::key(job->path, job->box);
    if (job->key.isEmpty()) {
      // Gone or unreadable for now; tried again when it is shown again.
      job->postFinished(Outcome::Retryable, false);
      return;
    }
    if (knownFailure(job->key)) {
      job->postFinished(Outcome::Failed, false);
      return;
    }
    if (job->knownKey != job->key) {
      job->known = {};
    }
    job->postKey();
    if (job->absorbCache()) {
      return;
    }
    // Of the storyboards waiting for the decoder, the newest goes first.
    const int priority = int(std::min<quint64>(job->serial, quint64(std::numeric_limits<int>::max())));
    extractionPool().start([job] { job->extract(); }, priority);
  });
}

bool Storyboard::cancel() {
  m_startTimer.stop();
  if (!m_job) {
    return false;
  }
  m_job->detach();
  m_job.reset();
  return true;
}

void Storyboard::clear() {
  ++m_generation;
  const bool hadFrames = !m_frames.isEmpty();
  const bool hadState = m_complete || m_failed || m_fromCache;
  m_frames = {};
  m_framesKey.clear();
  m_complete = false;
  m_failed = false;
  m_fromCache = false;
  renewToken();
  if (hadFrames) emit framesChanged();
  if (hadState) emit stateChanged();
}

void Storyboard::renewToken() {
  {
    QMutexLocker lock(&g_registryMutex);
    g_registry.remove(m_token);
  }
  m_token = QStringLiteral("%1-%2").arg(m_instance).arg(++m_tokens);
}

void Storyboard::publish() {
  auto snapshot = std::make_shared<const StoryboardFrames>(m_frames);
  QMutexLocker lock(&g_registryMutex);
  g_registry.insert(m_token, std::move(snapshot));
}

void Storyboard::acceptSample(quint64 generation, qint64 target, qint64 time,
                              const QByteArray& jpeg) {
  if (generation != m_generation) {
    return;
  }
  m_frames.markSampled(target);
  if (jpeg.isEmpty() || !m_frames.insert(time, jpeg)) {
    return;
  }
  publish();
  emit framesChanged();
}

void Storyboard::acceptCached(quint64 serial, const StoryboardFrames& frames, bool complete) {
  if (!m_job || m_job->serial != serial) {
    return;
  }
  const bool added = m_frames.merge(frames);
  if (complete) {
    m_job.reset();
    m_complete = true;
    m_fromCache = true;
  }
  if (added) {
    publish();
    emit framesChanged();
  }
  if (complete) {
    emit stateChanged();
  }
}

void Storyboard::adoptKey(quint64 serial, const QString& key) {
  if (!m_job || m_job->serial != serial || key == m_framesKey) {
    return;
  }
  // Another version of the file: its frames get URLs of their own, since Qt
  // caches images by URL and the old version's would otherwise come back.
  // The generation stays, so the job that found the key keeps reporting.
  const bool hadFrames = !m_frames.isEmpty();
  m_frames = {};
  m_framesKey = key;
  renewToken();
  if (hadFrames) {
    emit framesChanged();
  }
}

void Storyboard::finish(quint64 serial, Outcome outcome, bool complete) {
  if (!m_job || m_job->serial != serial) {
    return;
  }
  m_job.reset();
  switch (outcome) {
  case Outcome::Decoded:
    // A partial storyboard stays incomplete: the samples that timed out are
    // tried again the next time it starts.
    m_complete = complete;
    m_restartedForChange = false;
    break;
  case Outcome::Retryable:
    // Nothing to show yet, but nothing learned about the file either: shown
    // again, as when its window comes back to the front, it tries again.
    break;
  case Outcome::Failed:
    m_failed = m_frames.isEmpty();
    break;
  case Outcome::Changed: {
    // What was decoded belongs to the file as it was. Started once more for
    // the new version; a file still being written waits for the next change
    // of focus or source rather than being decoded over and over.
    const bool hadFrames = !m_frames.isEmpty();
    ++m_generation;
    m_frames = {};
    m_framesKey.clear();
    renewToken();
    if (hadFrames) emit framesChanged();
    if (!m_restartedForChange && m_active) {
      m_restartedForChange = true;
      m_startTimer.start();
    }
    break;
  }
  }
  emit stateChanged();
}
