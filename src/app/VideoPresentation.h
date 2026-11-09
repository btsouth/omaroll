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
class VideoPresentation : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(QVideoSink* videoSink READ videoSink CONSTANT)
  Q_PROPERTY(QString subtitleText READ subtitleText NOTIFY subtitleTextChanged)
  Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
  Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY readyChanged)
  Q_PROPERTY(QMediaPlayer* player READ player WRITE setPlayer NOTIFY playerChanged)
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
              if (QVideoSink* output = m_output.data()) output->setVideoFrame(frame);
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
  QMediaPlayer* player() const { return m_player; }
  void setPlayer(QMediaPlayer* player) {
    if (m_player == player) return;
    if (m_player) disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    m_stallTimer.stop();
    if (player) {
      connect(player, &QMediaPlayer::playbackStateChanged, this,
              &VideoPresentation::playbackStateChanged);
    }
    emit playerChanged();
  }
  Q_INVOKABLE void reset() {
    m_frameSeen = false;
    m_ready = false;
    m_stallTimer.stop();
    m_input.setSubtitleText({});
    releaseHeap();
    emit readyChanged();
  }
  // Call after changing the player's audio or subtitle track.
  Q_INVOKABLE void trackChanged() {
    // Audio-only media never delivers frames, so there is nothing to stall.
    if (!m_player || !m_player->hasVideo()
        || m_player->playbackState() != QMediaPlayer::PlayingState) {
      return;
    }
    m_stallClock.start();
    m_stallUntil = kStallWatchMs;
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
signals:
  void subtitleTextChanged();
  void readyChanged();
  void playerChanged();
private:
  void markReady() {
    // The scene graph can acquire its RHI after the player is attached.
    if (m_output && m_input.rhi() != m_output->rhi()) m_input.setRhi(m_output->rhi());
    if (!m_frameSeen) return; // reset() ran since this frame
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
  qint64 m_stallUntil = 0;
  qint64 m_stallProgress = 0;
  qint64 m_stallLimit = 0;
  int m_stallResyncs = 0;
  quint64 m_stallFrames = 0;
  bool m_ready = false;
  bool m_hasFrame = false;
  bool m_heapHeld = false;
};
