#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QPointer>
#include <QMutex>
#include <QSet>
#include <QQuickWindow>
#include <QVariant>
#include <QVideoFrame>
#include <QVideoSink>

#include <atomic>
#include <memory>
#include <utility>

// Opt-in developer diagnostics. No paths or media contents are logged.
class StartupTrace {
public:
  StartupTrace() : m_enabled(qEnvironmentVariableIntValue("OMAROLL_STARTUP_TRACE") == 1) {
    if (m_enabled) {
      m_clock.start();
      const StartupTrace* none = nullptr;
      s_active.compare_exchange_strong(none, this);
    }
  }
  ~StartupTrace() {
    const StartupTrace* self = this;
    s_active.compare_exchange_strong(self, nullptr);
  }
  StartupTrace(const StartupTrace& other) : m_enabled(other.m_enabled), m_clock(other.m_clock) {}
  StartupTrace& operator=(const StartupTrace&) = delete;

  void mark(const char* stage) const {
    if (m_enabled) {
      const QJsonObject event{{QStringLiteral("stage"), QString::fromLatin1(stage)},
                              {QStringLiteral("elapsed_ms"), m_clock.nsecsElapsed() / 1e6}};
      qInfo().noquote() << "OMAROLL_STARTUP"
                       << QJsonDocument(event).toJson(QJsonDocument::Compact);
    }
  }

  // A stage marked from outside main(), against the trace main() started,
  // once: the first time it is reached. Safe from any thread; nothing happens
  // when tracing is off.
  static void note(const char* stage) {
    const StartupTrace* active = s_active.load();
    if (!active) return;
    static QMutex mutex;
    static QSet<QByteArray> reported;
    {
      const QMutexLocker lock(&mutex);
      if (reported.contains(stage)) return;
      reported.insert(stage);
    }
    active->mark(stage);
  }

  void watch(QQuickWindow* window) const {
    if (!m_enabled || !window) {
      return;
    }
    struct State {
      std::atomic<int> ready{0};
      int synchronized = 0;
      int reported = 0;
      QPointer<QVideoSink> sink;
      bool decoded = false;
    };
    auto state = std::make_shared<State>();
    // Read QML on the GUI thread, then latch that snapshot during scene sync.
    // A later GUI update must not label an earlier frame as containing media.
    QObject::connect(window, &QQuickWindow::afterAnimating, window, [window, state, trace = *this] {
      QVariant ready;
      if (QMetaObject::invokeMethod(window, "startupReadiness", Q_RETURN_ARG(QVariant, ready))) {
        state->ready.store(ready.toInt());
      }
      // A track flag can precede decoded pixels by hundreds of milliseconds.
      // Observe the sink on the GUI thread, including an already-arrived frame.
      if (!state->sink && !state->decoded) {
        auto* player = window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"));
        if (player && player->videoSink()) {
          state->sink = player->videoSink();
          const auto decoded = [trace, state](const QVideoFrame& frame) {
            if (!state->decoded && frame.isValid()) {
              state->decoded = true;
              trace.mark("video_decoded_frame");
            }
          };
          QObject::connect(state->sink.data(), &QVideoSink::videoFrameChanged, window, decoded);
          // When the window thread learns the file is open; decoding
          // starts from here.
          QObject::connect(player, &QMediaPlayer::mediaStatusChanged, window,
                           [](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::LoadedMedia) note("media_loaded");
          });
          if (player->mediaStatus() == QMediaPlayer::LoadedMedia) note("media_loaded");
          decoded(state->sink->videoFrame());
        }
      }
    });
    QObject::connect(window, &QQuickWindow::sceneGraphInitialized, window, [trace = *this] {
      trace.mark("scenegraph");
    }, Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::beforeSynchronizing, window, [state] {
      state->synchronized = state->ready.load();
    }, Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::afterFrameEnd, window, [trace = *this, state] {
      const int frame = state->synchronized | 1;
      for (const auto& [bit, name] : {std::pair{1, "first_frame"},
                                     std::pair{2, "image_frame"},
                                     std::pair{4, "grid_frame"},
                                     std::pair{8, "poster_frame"},
                                     std::pair{16, "video_frame"}}) {
        if ((frame & bit) && !(state->reported & bit)) {
          trace.mark(name);
          state->reported |= bit;
        }
      }
    }, Qt::DirectConnection);
  }

private:
  bool m_enabled;
  QElapsedTimer m_clock;
  static inline std::atomic<const StartupTrace*> s_active = nullptr;
};
