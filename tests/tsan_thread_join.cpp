// Qt 6.12 joins its threads with pthread_clockjoin_np, which ThreadSanitizer
// does not intercept. TSan then never sees a Qt thread joined, keeps its
// handle registered, and aborts with a thread registry CHECK once glibc hands
// that handle to a new thread. Route the call through pthread_timedjoin_np,
// which TSan does intercept, so the join and its ordering are visible.
// Only TSan builds of the test binaries define it.

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define OMAROLL_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define OMAROLL_TSAN 1
#endif

#if defined(OMAROLL_TSAN) && defined(__linux__)
#include <cerrno>
#include <ctime>
#include <pthread.h>

extern "C" int pthread_clockjoin_np(pthread_t thread, void** result, clockid_t clock, const struct timespec* deadline) {
  if (deadline == nullptr) return pthread_join(thread, result);
  if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return EINVAL;
  if (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1'000'000'000) return EINVAL;
  if (clock == CLOCK_REALTIME) return pthread_timedjoin_np(thread, result, deadline);
  // pthread_timedjoin_np takes a CLOCK_REALTIME deadline.
  timespec now{};
  timespec wall{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  clock_gettime(CLOCK_REALTIME, &wall);
  long long nanoseconds = (deadline->tv_sec - now.tv_sec) * 1'000'000'000LL + (deadline->tv_nsec - now.tv_nsec);
  if (nanoseconds < 0) nanoseconds = 0;
  nanoseconds += wall.tv_nsec;
  timespec converted{};
  converted.tv_sec = wall.tv_sec + time_t(nanoseconds / 1'000'000'000LL);
  converted.tv_nsec = long(nanoseconds % 1'000'000'000LL);
  return pthread_timedjoin_np(thread, result, &converted);
}
#endif
