#pragma once

// Arch's Qt is not built with ThreadSanitizer, so TSan cannot see the mutexes
// Qt hands data across threads with: a queued call's posted event, a
// QtConcurrent task's start. Where a payload built on one thread is read on
// another through such a handoff, mark both ends with the payload's address.
// Each payload is its own token, so only that handoff is ordered. Both calls
// compile to nothing outside TSan builds.

#if defined(__SANITIZE_THREAD__)
#define OMAROLL_TSAN_HANDOFF 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define OMAROLL_TSAN_HANDOFF 1
#endif
#endif

#ifdef OMAROLL_TSAN_HANDOFF
extern "C" void __tsan_acquire(void* addr);
extern "C" void __tsan_release(void* addr);
#endif

namespace tsan {

// On the sending thread, after the payload is complete and before Qt hands
// it over.
inline void handOver([[maybe_unused]] const void* payload) {
#ifdef OMAROLL_TSAN_HANDOFF
  if (payload) __tsan_release(const_cast<void*>(payload));
#endif
}

// On the receiving thread, before the payload is read.
inline void takeOver([[maybe_unused]] const void* payload) {
#ifdef OMAROLL_TSAN_HANDOFF
  if (payload) __tsan_acquire(const_cast<void*>(payload));
#endif
}

} // namespace tsan
