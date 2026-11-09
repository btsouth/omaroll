#pragma once

#include "app/PlaybackHeap.h"

#include <QMediaPlayer>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QVideoSink>
#include <QVideoFrame>

#include <algorithm>
#include <atomic>
#include <cstring>

// Keep captions out of Qt Quick's video node. Only frames reach that sink;
// Omaroll's themed Text owns both embedded and sidecar captions.
//
// It also watches the player for two ways Qt's FFmpeg backend can leave the
// picture behind the clock:
// - A pause while a seek is still decoding holds the renderer before the
//   sought frame is drawn, so the poster or an old frame stays up. A paused
//   seek always draws one frame, so seek again to where the player says it is.
// - A track change rebuilds the decoders under a running clock and can stall
//   the picture for good, often with the clock. If frames stop, seek in place.
//   This also covers a change right after opening, when the stall can follow
//   a single new frame, so it times from the last frame, not a fixed window.
//
// And it says when a file gives nothing to show: loaded with no audio or
// video stream, or played to its end, or stuck, without a first frame. Qt
// reports no error for these and its clock runs on, so it would look like
// playback. A failed file is not played again until the next reset().

// How long a file has played without a first frame while its clock stood
// still. A clock that moves is content playing, such as audio before video
// that starts late, so that time does not count; neither does time paused.
class FirstFrameWatch {
public:
  void reset() {
    m_stalledMs = 0;
    m_lastTick = -1;
    m_lastPosition = -1;
  }
  // The stalled time so far, given the time now, whether the player is
  // playing and its position.
  qint64 update(qint64 now, bool playing, qint64 position) {
    if (!playing) {
      m_lastTick = -1;
      return m_stalledMs;
    }
    if (m_lastTick >= 0 && position <= m_lastPosition) m_stalledMs += now - m_lastTick;
    m_lastTick = now;
    m_lastPosition = position;
    return m_stalledMs;
  }
private:
  qint64 m_stalledMs = 0;
  qint64 m_lastTick = -1;
  qint64 m_lastPosition = -1;
};

class VideoPresentation : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(QVideoSink* videoSink READ videoSink CONSTANT)
  Q_PROPERTY(QString subtitleText READ subtitleText NOTIFY subtitleTextChanged)
  Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
  Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY readyChanged)
  Q_PROPERTY(QMediaPlayer* player READ player WRITE setPlayer NOTIFY playerChanged)
  // Why nothing can be shown, or empty. Cleared by reset().
  Q_PROPERTY(QString failure READ failure NOTIFY failureChanged)
  // Stalled play time allowed before a first frame; tests shorten it.
  Q_PROPERTY(int firstFrameTimeoutMs MEMBER m_firstFrameTimeoutMs)
public:
  // How far a paused picture may be from the reported position before it is
  // treated as stale. Generous: audio and video positions differ a little.
  static constexpr qint64 kPausedToleranceUs = 500'000;
  // After a track change, playing video that shows no new frame for this
  // long (or three frame durations, if longer) is stalled.
  static constexpr int kStallMs = 400;
  static constexpr int kStallPollMs = 100;
  // How long a track change is watched, and how long a resync may take.
  static constexpr int kStallWatchMs = 2400;
  static constexpr int kResyncGraceMs = 1200;
  static constexpr int kStallResyncs = 2;
  // Generous: a first frame normally takes well under a second, even for 4K
  // decoded in software or under a sanitizer, and this counts only time the
  // clock stood still.
  static constexpr int kFirstFrameMs = 10000;
  static constexpr int kOpenPollMs = 250;

  explicit VideoPresentation(QVideoSink* output, QQuickItem* surface = nullptr)
      : QQuickItem(surface), m_output(output) {
    connect(&m_input, &QVideoSink::subtitleTextChanged, this,
            &VideoPresentation::subtitleTextChanged);
    // Frames arrive on the decoder's thread. Hand them straight to the video
    // item's sink, as Qt's own VideoOutput does, so a busy GUI thread can't
    // delay or drop them; only the first frame after a source change is
    // reported to QML, on the GUI thread.
    connect(&m_input, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame& frame) {
              // Source changes clear the input. Retain the previous picture
              // until a new decoded frame (or a ready poster in QML) replaces it.
              if (!frame.isValid()) return;
              if (QVideoSink* output = m_output.data()) output->setVideoFrame(opaqueFrame(frame));
              m_frameStart.store(frame.startTime(), std::memory_order_relaxed);
              m_frameEnd.store(frame.endTime(), std::memory_order_relaxed);
              m_frames.fetch_add(1, std::memory_order_release);
              if (!m_frameSeen.exchange(true)) {
                QMetaObject::invokeMethod(this, &VideoPresentation::markReady, Qt::QueuedConnection);
              }
            },
            Qt::DirectConnection);
    m_stallTimer.setInterval(kStallPollMs);
    connect(&m_stallTimer, &QTimer::timeout, this, &VideoPresentation::checkStall);
    m_openTimer.setInterval(kOpenPollMs);
    connect(&m_openTimer, &QTimer::timeout, this, &VideoPresentation::checkOpen);
  }
  ~VideoPresentation() override { releaseHeap(); }
  Q_INVOKABLE QVideoSink* videoSink() {
    // Share the scene graph's RHI so hardware frames keep texture interop.
    if (m_output) m_input.setRhi(m_output->rhi());
    return &m_input;
  }
  QString subtitleText() const { return m_input.subtitleText(); }
  bool ready() const { return m_ready; }
  bool hasFrame() const { return m_hasFrame; }
  QString failure() const { return m_failure; }
  QMediaPlayer* player() const { return m_player; }
  void setPlayer(QMediaPlayer* player) {
    if (m_player == player) return;
    if (m_player) disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    m_stallTimer.stop();
    if (player) {
      connect(player, &QMediaPlayer::playbackStateChanged, this,
              &VideoPresentation::playbackStateChanged);
      connect(player, &QMediaPlayer::mediaStatusChanged, this, &VideoPresentation::checkOpen);
      connect(player, &QMediaPlayer::hasVideoChanged, this, &VideoPresentation::checkOpen);
      connect(player, &QMediaPlayer::hasAudioChanged, this, &VideoPresentation::checkOpen);
    }
    emit playerChanged();
    checkOpen();
  }
  Q_INVOKABLE void reset() {
    m_frameSeen = false;
    m_ready = false;
    m_stallTimer.stop();
    m_openTimer.stop();
    m_firstFrame.reset();
    m_noStreams.invalidate();
    m_input.setSubtitleText({});
    releaseHeap();
    emit readyChanged();
    if (!m_failure.isEmpty()) {
      m_failure.clear();
      emit failureChanged();
    }
  }
  // Call after changing the player's audio or subtitle track.
  Q_INVOKABLE void trackChanged() {
    // Audio-only media never delivers frames, so there is nothing to stall.
    if (!m_player || !m_player->hasVideo()
        || m_player->playbackState() != QMediaPlayer::PlayingState) {
      return;
    }
    m_stallClock.start();
    // Slow video needs room for its longer limit and one recovery seek.
    m_stallUntil = std::max<qint64>(kStallWatchMs, stallMs() + kResyncGraceMs + kStallMs);
    m_stallProgress = 0;
    m_stallLimit = 0;
    m_stallResyncs = 0;
    m_stallFrames = m_frames.load(std::memory_order_acquire);
    m_stallTimer.start();
  }
  // Whether the picture shown is the one at the player's position.
  bool showsPosition() const {
    if (!m_player || !m_frameSeen) return false;
    const qint64 at = m_player->position() * 1000;
    const qint64 start = m_frameStart.load(std::memory_order_relaxed);
    const qint64 end = std::max(start, m_frameEnd.load(std::memory_order_relaxed));
    return start >= 0 && at >= start - kPausedToleranceUs && at <= end + kPausedToleranceUs;
  }
  // Qt's shaders read the padding byte of RGB with one (FFmpeg's bgr0, rgb0,
  // 0rgb and 0bgr, as FFV1 stores RGB) as alpha, so a zero there draws the
  // picture blank or white. Such a frame gets its padding set opaque, in
  // place or in a copy; any other frame is returned as it is.
  static QVideoFrame opaqueFrame(const QVideoFrame& frame) {
    int padding = -1;
    switch (frame.pixelFormat()) {
    case QVideoFrameFormat::Format_XRGB8888:
    case QVideoFrameFormat::Format_XBGR8888:
      padding = 0;
      break;
    case QVideoFrameFormat::Format_RGBX8888:
    case QVideoFrameFormat::Format_BGRX8888:
      padding = 3;
      break;
    default:
      return frame;
    }
    const qsizetype width = qsizetype(frame.width()) * 4;
    // Whole pixels at a time, which the compiler vectorises; rows of 32-bit
    // pixels start 4-byte aligned.
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    const quint32 opaque = 0xffu << (8 * padding);
#else
    const quint32 opaque = 0xffu << (8 * (3 - padding));
#endif
    const auto fill = [opaque](uchar* row, qsizetype pixels) {
      auto* pixel = reinterpret_cast<quint32*>(row);
      for (qsizetype x = 0; x < pixels; ++x) pixel[x] |= opaque;
    };
    // In place where the frame's own buffer can be written, as an FFmpeg
    // software frame's can: no allocation and no copy. A backend that maps
    // a copy would lose the change, so it is read back.
    {
      QVideoFrame target(frame);
      if (target.map(QVideoFrame::ReadWrite)) {
        for (int y = 0; y < target.height(); ++y) {
          fill(target.bits(0) + qsizetype(y) * target.bytesPerLine(0), frame.width());
        }
        target.unmap();
        QVideoFrame check(frame);
        if (check.map(QVideoFrame::ReadOnly)) {
          const qsizetype last = qsizetype(check.height() - 1) * check.bytesPerLine(0) + width - 4 + padding;
          const bool written = check.bits(0)[padding] == 0xff && check.bits(0)[last] == 0xff;
          check.unmap();
          if (written) return frame;
        }
      }
    }
    QVideoFrame source(frame);
    if (!source.map(QVideoFrame::ReadOnly)) return frame;
    QVideoFrame copy(frame.surfaceFormat());
    if (!copy.isValid() || !copy.map(QVideoFrame::WriteOnly)) {
      source.unmap();
      return frame;
    }
    const int rows = std::min(source.height(), copy.height());
    const qsizetype bytes = qsizetype(std::min(source.width(), copy.width())) * 4;
    for (int y = 0; y < rows; ++y) {
      const uchar* from = source.bits(0) + qsizetype(y) * source.bytesPerLine(0);
      uchar* to = copy.bits(0) + qsizetype(y) * copy.bytesPerLine(0);
      std::memcpy(to, from, size_t(bytes));
      fill(to, bytes / 4);
    }
    source.unmap();
    copy.unmap();
    copy.setStartTime(frame.startTime());
    copy.setEndTime(frame.endTime());
    copy.setRotation(frame.rotation());
    copy.setMirrored(frame.mirrored());
    copy.setStreamFrameRate(frame.streamFrameRate());
    return copy;
  }
signals:
  void subtitleTextChanged();
  void readyChanged();
  void playerChanged();
  void failureChanged();
private:
  void markReady() {
    // The scene graph can acquire its RHI after the player is attached.
    if (m_output && m_input.rhi() != m_output->rhi()) m_input.setRhi(m_output->rhi());
    if (!m_frameSeen) return; // reset() ran since this frame
    m_openTimer.stop();
    if (!m_heapHeld) {
      // Frames are flowing: keep their buffers in the heap (PlaybackHeap.h).
      PlaybackHeap::hold();
      m_heapHeld = true;
    }
    const bool changed = !m_ready || !m_hasFrame;
    m_ready = true;
    m_hasFrame = true;
    if (changed) emit readyChanged();
  }
  void playbackStateChanged(QMediaPlayer::PlaybackState state) {
    if (state != QMediaPlayer::PlayingState) m_stallTimer.stop();
    // A failed file stays stopped: a tap, a key or MPRIS Play would run its
    // clock again under the error.
    if (state == QMediaPlayer::PlayingState && !m_failure.isEmpty() && m_player) {
      QMetaObject::invokeMethod(m_player.data(), &QMediaPlayer::stop, Qt::QueuedConnection);
      return;
    }
    checkOpen();
    // Runs inside pause(), before callers seek elsewhere, so only a picture
    // that is already stale is sought again.
    if (state != QMediaPlayer::PausedState || !m_player || !m_player->isSeekable()
        || !m_player->hasVideo()) {
      return;
    }
    const auto status = m_player->mediaStatus();
    if (status == QMediaPlayer::NoMedia || status == QMediaPlayer::LoadingMedia
        || status == QMediaPlayer::InvalidMedia || status == QMediaPlayer::EndOfMedia) {
      return;
    }
    if (!showsPosition()) resync();
  }
  void checkStall() {
    const qint64 now = m_stallClock.elapsed();
    if (!m_player || m_player->playbackState() != QMediaPlayer::PlayingState
        || now >= m_stallUntil) {
      m_stallTimer.stop();
      return;
    }
    const quint64 frames = m_frames.load(std::memory_order_acquire);
    if (frames != m_stallFrames) {
      m_stallFrames = frames;
      m_stallProgress = now;
      m_stallLimit = 0;
      return;
    }
    // The clock can stall with the picture, so time from the last new frame
    // rather than wait for the clock to move. A seek gets longer to show one.
    const qint64 limit = m_stallLimit > 0 ? m_stallLimit : stallMs();
    if (now - m_stallProgress < limit) return;
    if (m_stallResyncs >= kStallResyncs) {
      m_stallTimer.stop();
      return;
    }
    ++m_stallResyncs;
    m_stallProgress = now;
    m_stallLimit = kResyncGraceMs;
    m_stallUntil = std::max(m_stallUntil, now + kResyncGraceMs + kStallMs * 2);
    resync();
  }
  void fail(const QString& reason) {
    m_openTimer.stop();
    if (m_failure == reason) return;
    m_failure = reason;
    emit failureChanged();
  }
  // Watches a newly opened file until its first frame.
  void checkOpen() {
    if (!m_player || m_frameSeen || !m_failure.isEmpty()) {
      m_openTimer.stop();
      return;
    }
    const auto status = m_player->mediaStatus();
    const bool loaded = status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferingMedia
                        || status == QMediaPlayer::BufferedMedia || status == QMediaPlayer::EndOfMedia;
    const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;
    // Seen twice a poll apart, so a backend that reports its streams just
    // after the load is not failed.
    if (loaded && !m_player->hasVideo() && !m_player->hasAudio()) {
      if (!m_noStreams.isValid()) {
        m_noStreams.start();
        QTimer::singleShot(kOpenPollMs, Qt::PreciseTimer, this, &VideoPresentation::checkOpen);
      } else if (m_noStreams.elapsed() >= kOpenPollMs) {
        fail(QStringLiteral("No video or audio stream could be read"));
      }
      return;
    }
    m_noStreams.invalidate();
    // Audio alone plays without frames.
    const bool watched = m_player->hasVideo() || !m_player->hasAudio();
    if (watched && status == QMediaPlayer::EndOfMedia) {
      fail(QStringLiteral("No picture came from this video"));
      return;
    }
    if (!m_openClock.isValid()) m_openClock.start();
    const qint64 stalled = m_firstFrame.update(m_openClock.elapsed(), playing && watched, m_player->position());
    if (!playing || !watched) {
      m_openTimer.stop();
      return;
    }
    if (!m_openTimer.isActive()) m_openTimer.start();
    if (stalled >= m_firstFrameTimeoutMs) fail(QStringLiteral("No picture came from this video"));
  }
  // Slow video may go longer than kStallMs between frames.
  qint64 stallMs() const {
    const qint64 frameUs = m_frameEnd.load(std::memory_order_relaxed)
                           - m_frameStart.load(std::memory_order_relaxed);
    const qreal rate = m_player ? std::max<qreal>(0.25, m_player->playbackRate()) : 1.0;
    return std::max<qint64>(kStallMs, qint64(3 * frameUs / 1000 / rate));
  }
  void resync() {
    if (!m_player) return;
    m_player->setPosition(m_player->position());
  }
  void releaseHeap() {
    if (!m_heapHeld) return;
    m_heapHeld = false;
    PlaybackHeap::release();
  }
  std::atomic_bool m_frameSeen = false;
  std::atomic<qint64> m_frameStart = -1;
  std::atomic<qint64> m_frameEnd = -1;
  std::atomic<quint64> m_frames = 0;
  QVideoSink m_input;
  QPointer<QVideoSink> m_output;
  QPointer<QMediaPlayer> m_player;
  QTimer m_stallTimer;
  QElapsedTimer m_stallClock;
  QTimer m_openTimer;
  QElapsedTimer m_openClock;
  QElapsedTimer m_noStreams;
  FirstFrameWatch m_firstFrame;
  int m_firstFrameTimeoutMs = kFirstFrameMs;
  QString m_failure;
  qint64 m_stallUntil = 0;
  qint64 m_stallProgress = 0;
  qint64 m_stallLimit = 0;
  int m_stallResyncs = 0;
  quint64 m_stallFrames = 0;
  bool m_ready = false;
  bool m_hasFrame = false;
  bool m_heapHeld = false;
};
