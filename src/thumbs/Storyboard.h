#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

class QQmlEngine;

// Seek-bar previews. A storyboard is a bounded set of keyframes from one video,
// decoded once in the background and kept on disk, so hovering the seek bar
// shows the frame nearest the pointer without asking the player, or any
// decoder, for anything.
namespace StoryboardPlan {

// A shorter clip scrubs fine on its live frame, and a handful of previews
// would say less than the time already does.
inline constexpr qint64 kMinimumDurationMs = 5000;
inline constexpr int kMaximumFrames = 100;
inline constexpr qint64 kMinimumIntervalMs = 2000;
// The logical box a preview fits in. A 16:9 frame fills it.
inline constexpr int kBoxWidth = 192;
inline constexpr int kBoxHeight = 108;

// Where to sample: the middle of each of up to kMaximumFrames equal slots,
// each at least kMinimumIntervalMs long. Empty when the clip is too short.
[[nodiscard]] QList<qint64> sampleTimes(qint64 durationMs);
// The frame box in device pixels. Two sizes only, 1x and 2x, so a fractional
// scale shares a cache entry with its neighbours instead of re-extracting.
[[nodiscard]] QSize pixelBox(qreal devicePixelRatio);
// Coarse to fine: the first few frames already span the whole clip, so a
// partial storyboard is useful while the rest is still being decoded.
[[nodiscard]] QList<int> extractionOrder(int count);
// Index of the time closest to `at`, or -1 for an empty list.
[[nodiscard]] int nearest(const QList<qint64>& sortedTimes, qint64 at);
// How far a sample tried for another plan, say for a duration reported a
// few milliseconds differently, may be from a target and still stand in for
// it: a quarter of a slot, which lands on the same keyframe or one as near.
[[nodiscard]] qint64 tolerance(const QList<qint64>& times);

} // namespace StoryboardPlan

// Frames as decoded JPEG bytes, ordered by their real timestamps. Two samples
// that land on the same keyframe are one frame. `sampled` lists the sample
// times already tried to the end, with or without a frame, so a storyboard
// that was cut short or timed out fills in only the rest. It is `complete`
// once every planned sample has been tried; that is worked out again against
// the current plan whenever it matters, since the plan follows the duration.
struct StoryboardFrames {
  QSize frameSize;
  QList<qint64> times;
  QList<QByteArray> jpegs;
  QList<qint64> sampled;
  bool complete = false;

  bool insert(qint64 time, const QByteArray& jpeg);
  void markSampled(qint64 target);
  [[nodiscard]] bool wasSampled(qint64 target, qint64 tolerance = 0) const;
  // Every one of `times` was tried, within `tolerance`.
  [[nodiscard]] bool covers(const QList<qint64>& times, qint64 tolerance) const;
  // Drops the samples that belong to no target of `times`.
  void keepSampled(const QList<qint64>& times, qint64 tolerance);
  // Adds the other's frames and samples. True when a frame was added.
  bool merge(const StoryboardFrames& other);
  [[nodiscard]] bool isEmpty() const { return times.isEmpty(); }
};

namespace StoryboardCache {

// A full storyboard is about half a megabyte at 2x, so this keeps more than
// a hundred videos. Always rebuildable, like the thumbnail cache.
inline constexpr qint64 kMaximumBytes = 64LL * 1024 * 1024;

// A save in progress takes well under a second. A staging file older than
// this was left by a process that died while saving.
inline constexpr qint64 kAbandonedStagingMs = 10LL * 60 * 1000;

// Owner-only, like the frames it holds: a private video's frames are as
// private as the video.
[[nodiscard]] QString directory();
// Keyed by path, size, mtime and ctime, and the frame box. Empty when the
// file cannot be read. This stats the file, so call it off the GUI thread.
[[nodiscard]] QString key(const QString& path, const QSize& pixelBox);
// A hit counts as a use for eviction. A partial entry loads too; check
// `complete`. An entry with a frame that does not decode is a miss, so it is
// decoded again rather than shown blank.
[[nodiscard]] std::optional<StoryboardFrames> load(const QString& key);
bool save(const QString& key, const StoryboardFrames& frames);
// Least recently used first. Staging files left by a process that died while
// saving count toward the limit and are removed once abandoned.
void prune(qint64 maximumBytes = kMaximumBytes);
#ifdef OMAROLL_FUZZ
// Loads |bytes| as a cache entry and checks what a successful load returns.
void fuzzLoad(const QByteArray& bytes);
#endif

} // namespace StoryboardCache

struct StoryboardJob;

// One video's storyboard, for QML. Extraction waits until `active` (the first
// frame is on screen in the focused window), holds new decodes while `paused`
// (the user is seeking), and stops when the source or its `version` changes,
// `active` goes false or the object goes away. A file that changes on disk
// while it is being decoded starts over rather than mixing two versions. Whatever was decoded by then is kept, in
// memory and in the cache, and the next run decodes only the samples still
// missing. Frames are served by the "storyboard" image provider at the URLs
// frameUrl() gives.
class Storyboard : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
  // Anything that changes when the file does, such as the library's
  // thumbnail version. Optional: the file's own version is checked anyway
  // each time extraction starts.
  Q_PROPERTY(QString version READ version WRITE setVersion NOTIFY versionChanged)
  Q_PROPERTY(qreal duration READ duration WRITE setDuration NOTIFY durationChanged)
  Q_PROPERTY(qreal devicePixelRatio READ devicePixelRatio WRITE setDevicePixelRatio
                 NOTIFY devicePixelRatioChanged)
  Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
  Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
  Q_PROPERTY(int frameCount READ frameCount NOTIFY framesChanged)
  Q_PROPERTY(qreal aspectRatio READ aspectRatio NOTIFY framesChanged)
  Q_PROPERTY(bool complete READ complete NOTIFY stateChanged)
  Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
  Q_PROPERTY(bool running READ running NOTIFY stateChanged)
  Q_PROPERTY(bool fromCache READ fromCache NOTIFY stateChanged)

public:
  static constexpr const char* kProviderId = "storyboard";
  // How long the conditions to start must hold. Rapid flipping through a
  // folder never starts a decoder, and the player has settled on the new
  // file's duration by then.
  static constexpr int kStartDelayMs = 300;
  // Decoder processes per storyboard, and one storyboard extracts at a time.
  static constexpr int kParallelProcesses = 2;

  explicit Storyboard(QObject* parent = nullptr);
  ~Storyboard() override;

  // Registers the QML type and adds the image provider to this engine.
  static void install(QQmlEngine& engine);
  // Stops every extraction and waits a moment for the workers. For
  // application exit: a worker stuck on a stalled mount is left behind
  // rather than waited for, and the process exits without it.
  static void shutdown();
  // Waits for the workers of storyboards already gone to finish saving.
  // For teardown in tests and tools, before the cache moves or the process
  // ends; a storyboard still extracting keeps it waiting.
  static void waitForWorkers();

  // Process counters, for tests and measurements.
  [[nodiscard]] static int runningProcesses();
  [[nodiscard]] static int launchedProcesses();
#ifdef OMAROLL_TESTING
  // How long one sample may take before it is given up for this run.
  static void setFrameTimeoutMs(int milliseconds);
  // How long a file ffmpeg turned down is left alone.
  static void setFailureMemoryMs(int milliseconds);
#endif

  [[nodiscard]] QString source() const { return m_source; }
  void setSource(const QString& source);
  [[nodiscard]] QString version() const { return m_version; }
  void setVersion(const QString& version);
  [[nodiscard]] qreal duration() const { return m_duration; }
  void setDuration(qreal duration);
  [[nodiscard]] qreal devicePixelRatio() const { return m_devicePixelRatio; }
  void setDevicePixelRatio(qreal ratio);
  [[nodiscard]] bool active() const { return m_active; }
  void setActive(bool active);
  [[nodiscard]] bool paused() const { return m_paused; }
  void setPaused(bool paused);

  [[nodiscard]] int frameCount() const { return int(m_frames.times.size()); }
  [[nodiscard]] qreal aspectRatio() const;
  [[nodiscard]] bool complete() const { return m_complete; }
  [[nodiscard]] bool failed() const { return m_failed; }
  [[nodiscard]] bool running() const { return bool(m_job); }
  [[nodiscard]] bool fromCache() const { return m_fromCache; }

  Q_INVOKABLE int frameAt(qreal milliseconds) const;
  Q_INVOKABLE qreal frameTime(int index) const;
  Q_INVOKABLE QString frameUrl(int index) const;

signals:
  void sourceChanged();
  void versionChanged();
  void durationChanged();
  void devicePixelRatioChanged();
  void activeChanged();
  void pausedChanged();
  void framesChanged();
  void stateChanged();

public:
  // How a run ended.
  enum class Outcome {
    Decoded,   // frames, complete or not
    Retryable, // nothing yet, but only for now: timeouts, crashes, I/O
    Failed,    // ffmpeg turned the file down: no video it can read
    Changed,   // the file changed on disk while it was being decoded
  };

private:
  friend struct StoryboardJob;
  void reconsider();
  void start();
  bool cancel();
  void clear();
  void renewToken();
  void publish();
  void acceptSample(quint64 generation, qint64 target, qint64 time, const QByteArray& jpeg);
  void acceptCached(quint64 serial, const StoryboardFrames& frames, bool complete);
  void adoptKey(quint64 serial, const QString& key);
  void finish(quint64 serial, Outcome outcome, bool complete);

  QString m_source;
  QString m_version;
  qreal m_duration = 0;
  qreal m_devicePixelRatio = 1;
  bool m_active = false;
  bool m_paused = false;

  QString m_identity;
  // The sample times for the current duration.
  QList<qint64> m_plan;
  // Names this object's frames for the image provider, and changes with the
  // file version, so Qt's image cache never serves another version's frame.
  QString m_token;
  quint64 m_instance = 0;
  quint64 m_tokens = 0;
  quint64 m_generation = 0;
  // A run that ended because the file changed is started again once; a file
  // still being written waits until something else changes.
  bool m_restartedForChange = false;
  StoryboardFrames m_frames;
  // The file version m_frames came from. A file rewritten under the same
  // path starts over rather than mixing old frames with new.
  QString m_framesKey;
  bool m_complete = false;
  bool m_failed = false;
  bool m_fromCache = false;
  std::shared_ptr<StoryboardJob> m_job;
  QTimer m_startTimer;
};
