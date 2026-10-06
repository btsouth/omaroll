#pragma once

#include <QGuiApplication>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>

#include <memory>

namespace ClipboardFallback {

// A worker must not wait forever if the GUI event loop is closing. A timed-out
// offer is canceled before a queued callback can change the clipboard.
template <typename Offer>
bool onGuiThread(Offer offer) {
  auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
  if (!application) return false;
  if (QThread::currentThread() == application->thread()) return offer();

  struct State {
    QMutex mutex;
    QWaitCondition finished;
    bool active = true;
    bool done = false;
    bool offered = false;
  };
  auto state = std::make_shared<State>();
  QMutexLocker lock(&state->mutex);
  if (!QMetaObject::invokeMethod(application, [state, offer] {
        QMutexLocker callbackLock(&state->mutex);
        if (!state->active) return;
        state->offered = offer();
        state->done = true;
        state->finished.wakeAll();
      }, Qt::QueuedConnection)) {
    return false;
  }
  if (!state->done) state->finished.wait(&state->mutex, 1000);
  state->active = false;
  return state->done && state->offered;
}

} // namespace ClipboardFallback
