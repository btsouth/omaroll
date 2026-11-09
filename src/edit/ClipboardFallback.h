#pragma once

#include <QGuiApplication>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace ClipboardFallback {

// A worker must not wait forever if the GUI event loop is closing. A timed-out
// offer is canceled before a queued callback can change the clipboard.
template <typename Offer>
bool onGuiThread(Offer offer) {
  auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
  if (!application) return false;
  if (QThread::currentThread() == application->thread()) return offer();

  // Standard primitives rather than QMutex/QWaitCondition: the wait releases
  // the lock inside libstdc++, where ThreadSanitizer can see it.
  struct State {
    std::mutex mutex;
    std::condition_variable finished;
    bool active = true;
    bool done = false;
    bool offered = false;
  };
  auto state = std::make_shared<State>();
  std::unique_lock lock(state->mutex);
  if (!QMetaObject::invokeMethod(application, [state, offer] {
        std::lock_guard callbackLock(state->mutex);
        if (!state->active) return;
        state->offered = offer();
        state->done = true;
        state->finished.notify_all();
      }, Qt::QueuedConnection)) {
    return false;
  }
  state->finished.wait_for(lock, std::chrono::seconds(1), [&state] { return state->done; });
  state->active = false;
  return state->done && state->offered;
}

} // namespace ClipboardFallback
