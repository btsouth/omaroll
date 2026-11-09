#pragma once

#include <QQuickItem>
#include <QPointer>
#include <QVideoSink>
#include <QVideoFrame>

#include <atomic>

// Keep captions out of Qt Quick's video node. Only frames reach that sink;
// Omaroll's themed Text owns both embedded and sidecar captions.
class VideoPresentation : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(QVideoSink* videoSink READ videoSink CONSTANT)
  Q_PROPERTY(QString subtitleText READ subtitleText NOTIFY subtitleTextChanged)
  Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
  Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY readyChanged)
public:
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
              if (!m_frameSeen.exchange(true)) {
                QMetaObject::invokeMethod(this, &VideoPresentation::markReady, Qt::QueuedConnection);
              }
            },
            Qt::DirectConnection);
  }
  Q_INVOKABLE QVideoSink* videoSink() {
    // Share the scene graph's RHI so hardware frames keep texture interop.
    if (m_output) m_input.setRhi(m_output->rhi());
    return &m_input;
  }
  QString subtitleText() const { return m_input.subtitleText(); }
  bool ready() const { return m_ready; }
  bool hasFrame() const { return m_hasFrame; }
  Q_INVOKABLE void reset() {
    m_frameSeen = false;
    m_ready = false;
    m_input.setSubtitleText({});
    emit readyChanged();
  }
signals:
  void subtitleTextChanged();
  void readyChanged();
private:
  void markReady() {
    // The scene graph can acquire its RHI after the player is attached.
    if (m_output && m_input.rhi() != m_output->rhi()) m_input.setRhi(m_output->rhi());
    if (!m_frameSeen) return; // reset() ran since this frame
    const bool changed = !m_ready || !m_hasFrame;
    m_ready = true;
    m_hasFrame = true;
    if (changed) emit readyChanged();
  }
  std::atomic_bool m_frameSeen = false;
  QVideoSink m_input;
  QPointer<QVideoSink> m_output;
  bool m_ready = false;
  bool m_hasFrame = false;
};
