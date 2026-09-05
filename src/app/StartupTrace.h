#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QVariant>

#include <atomic>
#include <memory>
#include <utility>

// Opt-in developer diagnostics. No paths or media contents are logged.
class StartupTrace {
public:
  StartupTrace() : m_enabled(qEnvironmentVariableIntValue("OMAROLL_STARTUP_TRACE") == 1) {
    if (m_enabled) {
      m_clock.start();
    }
  }

  void mark(const char* stage) const {
    if (m_enabled) {
      const QJsonObject event{{QStringLiteral("stage"), QString::fromLatin1(stage)},
                              {QStringLiteral("elapsed_ms"), m_clock.nsecsElapsed() / 1e6}};
      qInfo().noquote() << "OMAROLL_STARTUP"
                       << QJsonDocument(event).toJson(QJsonDocument::Compact);
    }
  }

  void watch(QQuickWindow* window) const {
    if (!m_enabled || !window) {
      return;
    }
    struct State {
      std::atomic<int> ready{0};
      int synchronized = 0;
      int reported = 0;
    };
    auto state = std::make_shared<State>();
    // Read QML on the GUI thread, then latch that snapshot during scene sync.
    // A later GUI update must not label an earlier frame as containing media.
    QObject::connect(window, &QQuickWindow::afterAnimating, window, [window, state] {
      QVariant ready;
      if (QMetaObject::invokeMethod(window, "startupReadiness", Q_RETURN_ARG(QVariant, ready))) {
        state->ready.store(ready.toInt());
      }
    });
    QObject::connect(window, &QQuickWindow::beforeSynchronizing, window, [state] {
      state->synchronized = state->ready.load();
    }, Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::afterFrameEnd, window, [trace = *this, state] {
      const int frame = state->synchronized | 1;
      for (const auto& [bit, name] : {std::pair{1, "first_frame"},
                                     std::pair{2, "image_frame"},
                                     std::pair{4, "grid_frame"}}) {
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
};
