#pragma once

#include <QObject>
#include <QPointer>
#include <QVideoSink>
#include <QVideoFrame>

// Keep captions out of Qt Quick's video node. Only frames reach that sink;
// Omaroll's themed Text owns both embedded and sidecar captions.
class VideoPresentation : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVideoSink* videoSink READ videoSink CONSTANT)
  Q_PROPERTY(QString subtitleText READ subtitleText NOTIFY subtitleTextChanged)
  Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
  Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY readyChanged)
public:
  explicit VideoPresentation(QVideoSink* output) : QObject(output), m_output(output) {
    connect(&m_input, &QVideoSink::subtitleTextChanged, this,
            &VideoPresentation::subtitleTextChanged);
    connect(&m_input, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
      // Source changes clear the input. Retain the previous picture until a
      // new decoded frame (or a ready poster in QML) can replace it.
      if (!frame.isValid()) return;
      if (m_output) {
        // The scene graph can acquire its RHI after the player is attached.
        if (m_input.rhi() != m_output->rhi()) m_input.setRhi(m_output->rhi());
        m_output->setVideoFrame(frame);
      }
      const bool changed = !m_ready || !m_hasFrame;
      m_ready = true;
      m_hasFrame = true;
      if (changed) emit readyChanged();
    });
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
    m_ready = false;
    m_input.setSubtitleText({});
    emit readyChanged();
  }
signals:
  void subtitleTextChanged();
  void readyChanged();
private:
  QVideoSink m_input;
  QPointer<QVideoSink> m_output;
  bool m_ready = false;
  bool m_hasFrame = false;
};
