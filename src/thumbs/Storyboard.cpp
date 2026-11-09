#include "thumbs/Storyboard.h"

#include "sources/FileVersion.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
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
#include <QSet>
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

bool StoryboardFrames::wasSampled(qint64 target) const {
  return std::binary_search(sampled.cbegin(), sampled.cend(), target);
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
// 2 added the sampled times and the complete flag.
constexpr quint16 kFormat = 2;
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
    qint64 time = 0;
    QByteArray jpeg;
    in >> time >> jpeg;
    if (in.status() != QDataStream::Ok || jpeg.isEmpty() || jpeg.size() > kMaximumFrameBytes ||
        (!frames.times.isEmpty() && time <= frames.times.constLast()) || time < 0) {
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

bool StoryboardCache::save(const QString& key, const StoryboardFrames& frames) {
  if (key.isEmpty() || frames.isEmpty() || !frames.frameSize.isValid()) {
    return false;
  }
  const QString path = entryPath(key);
  QDir().mkpath(directory());
  // Written beside and renamed into place, so a reader sees a whole file or none.
  const QString staging = path + QLatin1Char('.') +
                          QString::number(QCoreApplication::applicationPid()) + QLatin1Char('.') +
                          QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId())) +
                          QStringLiteral(".tmp");
  QFile file(staging);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
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
  if (!written || ::rename(QFile::encodeName(staging).constData(),
                           QFile::encodeName(path).constData()) != 0) {
    QFile::remove(staging);
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

// Files with no video, or none ffmpeg can read, until they change.
QMutex g_failedMutex;
QSet<QString> g_failed;

bool knownFailure(const QString& key) {
  QMutexLocker lock(&g_failedMutex);
  return g_failed.contains(key);
}

void rememberFailure(const QString& key) {
  QMutexLocker lock(&g_failedMutex);
  g_failed.insert(key);
}

// One storyboard decodes at a time; cache reads have their own lane so a
// cached video opens with previews while another is still extracting. Of the
// storyboards waiting for the decoder, the newest goes first. A window that
// loses focus cancels its own, so the one in front never waits behind it.
QThreadPool& extractionPool() {
  static QThreadPool pool;
  static const bool configured = [] {
    pool.setMaxThreadCount(1);
    return true;
  }();
  Q_UNUSED(configured);
  return pool;
}

QThreadPool& cachePool() {
  static QThreadPool pool;
  static const bool configured = [] {
    pool.setMaxThreadCount(2);
    return true;
  }();
  Q_UNUSED(configured);
  return pool;
}

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
      QStringLiteral("-map"), QStringLiteral("0:v:0"),
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
                     "\\bpts_time:\\s*(-?[0-9]+(?:\\.[0-9]+)?)"),
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
    post([generation = generation, target, time, jpeg](Storyboard* owner_) {
      owner_->acceptSample(generation, target, time, jpeg);
    });
  }

  void postKey() {
    post([serial = serial, key = key](Storyboard* owner_) { owner_->adoptKey(serial, key); });
  }

  void postCached(const StoryboardFrames& frames) {
    post([serial = serial, frames](Storyboard* owner_) { owner_->acceptCached(serial, frames); });
  }

  void postFinished(bool succeeded, bool complete) {
    post([serial = serial, succeeded, complete](Storyboard* owner_) {
      owner_->finish(serial, succeeded, complete);
    });
  }

  // Folds the cache entry, if any, into `known` and tells the owner. True
  // when the entry was complete, which leaves nothing to decode.
  bool absorbCache() {
    std::optional<StoryboardFrames> cached = StoryboardCache::load(key);
    if (!cached) {
      return false;
    }
    if (known.merge(*cached) || cached->complete) {
      postCached(*cached);
    }
    return cached->complete;
  }

  [[nodiscard]] bool allSampled(const StoryboardFrames& frames) const {
    return std::all_of(times.cbegin(), times.cend(),
                       [&frames](qint64 target) { return frames.wasSampled(target); });
  }

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
    postFinished(false, false);
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
    if (!frames.wasSampled(times.at(index))) {
      pending.append(times.at(index));
    }
  }
  qsizetype next = 0;
  int attempts = 0;
  int timeouts = 0;
  int tried = 0;
  const int frameTimeoutMs = g_frameTimeoutMs.load();

  const auto stop = [](Running& item) {
    if (item.process->state() != QProcess::NotRunning) {
      item.process->kill();
      item.process->waitForFinished(1000);
    }
    g_running.fetch_sub(1);
  };

  while (!isCancelled()) {
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
        const bool okay = process.exitStatus() == QProcess::NormalExit &&
                          process.exitCode() == 0 && jpeg.startsWith("\xff\xd8");
        if (!isCancelled()) {
          // Tried to the end either way; a sample ffmpeg turns down would
          // be turned down again.
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
        ++timeouts;
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
  frames.complete = allSampled(frames);
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
    // A file ffmpeg turned down is not tried again until it changes. One
    // that was only slow, on a busy machine, is tried next time it opens.
    if (timeouts == 0) {
      rememberFailure(key);
    }
    postFinished(false, false);
    return;
  }
  postFinished(true, frames.complete);
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
  cachePool().waitForDone();
  extractionPool().waitForDone();
}

int Storyboard::runningProcesses() { return g_running.load(); }
int Storyboard::launchedProcesses() { return g_launched.load(); }

#ifdef OMAROLL_TESTING
void Storyboard::setFrameTimeoutMs(int milliseconds) { g_frameTimeoutMs.store(milliseconds); }
#endif

void Storyboard::setSource(const QString& source) {
  if (m_source == source) return;
  m_source = source;
  emit sourceChanged();
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
  const QString identity = eligible ? m_source + QLatin1Char('|') + QString::number(box.width()) +
                                          QLatin1Char('x') + QString::number(box.height())
                                    : QString();
  if (identity != m_identity) {
    cancel();
    clear();
    m_identity = identity;
  }
  if (identity.isEmpty() || m_complete || m_failed) {
    m_startTimer.stop();
    return;
  }
  if (!m_active) {
    m_startTimer.stop();
    // Whatever was decoded so far stays on show, and the job saves it, but no
    // decoder runs for a viewer that is closed, behind another window or
    // showing something else.
    if (m_job) {
      cancel();
      emit stateChanged();
    }
    return;
  }
  if (!m_job && !m_startTimer.isActive()) {
    m_startTimer.start();
  }
}

void Storyboard::start() {
  if (m_job || m_identity.isEmpty() || !m_active || m_complete || m_failed) {
    return;
  }
  auto job = std::make_shared<StoryboardJob>();
  job->source = m_source;
  job->box = StoryboardPlan::pixelBox(m_devicePixelRatio);
  job->times = StoryboardPlan::sampleTimes(qint64(m_duration));
  job->known = m_frames;
  job->knownKey = m_framesKey;
  job->generation = m_generation;
  job->serial = ++g_jobs;
  job->paused.store(m_paused);
  job->attach(this);
  if (job->times.isEmpty()) {
    m_failed = true;
    emit stateChanged();
    return;
  }
  m_job = job;
  m_fromCache = false;
  emit stateChanged();

  cachePool().start([job] {
    if (job->isCancelled()) {
      return;
    }
    job->path = QFileInfo(job->source).absoluteFilePath();
    job->key = StoryboardCache::key(job->path, job->box);
    if (job->key.isEmpty() || knownFailure(job->key)) {
      job->postFinished(false, false);
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

void Storyboard::cancel() {
  m_startTimer.stop();
  if (m_job) {
    m_job->detach();
    m_job.reset();
  }
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
  {
    QMutexLocker lock(&g_registryMutex);
    g_registry.remove(m_token);
  }
  m_token = QStringLiteral("%1-%2").arg(m_instance).arg(m_generation);
  if (hadFrames) emit framesChanged();
  if (hadState) emit stateChanged();
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

void Storyboard::acceptCached(quint64 serial, const StoryboardFrames& frames) {
  if (!m_job || m_job->serial != serial) {
    return;
  }
  const bool added = m_frames.merge(frames);
  if (frames.complete) {
    m_job.reset();
    m_complete = true;
    m_fromCache = true;
  }
  if (added) {
    publish();
    emit framesChanged();
  }
  if (frames.complete) {
    emit stateChanged();
  }
}

void Storyboard::adoptKey(quint64 serial, const QString& key) {
  if (!m_job || m_job->serial != serial || key == m_framesKey) {
    return;
  }
  const bool hadFrames = !m_frames.isEmpty();
  m_frames = {};
  m_framesKey = key;
  if (hadFrames) {
    publish();
    emit framesChanged();
  }
}

void Storyboard::finish(quint64 serial, bool succeeded, bool complete) {
  if (!m_job || m_job->serial != serial) {
    return;
  }
  m_job.reset();
  // A partial storyboard stays incomplete: the samples that timed out are
  // tried again the next time it starts.
  m_complete = succeeded && complete;
  m_failed = !succeeded && m_frames.isEmpty();
  emit stateChanged();
}
