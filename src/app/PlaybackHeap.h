#pragma once

#include <mutex>

#ifdef __GLIBC__
#include <malloc.h>
#endif

// glibc allocator thresholds. Decoded pictures are freed straight back to the
// system: glibc otherwise raises its mmap threshold after the first large free
// and keeps later pictures in its heaps, so browsing a folder only ever grows.
//
// Video is the exception. Qt copies every hardware-decoded frame into a new
// buffer (12 MB for 4K 8-bit, 25 MB for 10-bit). Above the picture threshold
// each one is mapped, faulted in page by page and unmapped again, 60 times a
// second, which doubled the cost of 4K playback. While any video shows
// frames, those buffers are reused from the heap instead. The top pad keeps
// glibc from unmapping a thread's heap each time its last frame is freed,
// which otherwise brought the faults back on about half of all launches.
// Everything is restored and the heap trimmed once the last video goes.
class PlaybackHeap {
public:
  static constexpr int pictureMmapThreshold = 4 * 1024 * 1024;
  // glibc's largest accepted mmap threshold on 64-bit, and room to keep a
  // few freed frames before trimming.
  static constexpr int videoMmapThreshold = 32 * 1024 * 1024;
  static constexpr int videoTrimThreshold = 64 * 1024 * 1024;
  static constexpr int videoTopPad = 64 * 1024 * 1024; // a whole thread heap
  // glibc's defaults.
  static constexpr int pictureTrimThreshold = 128 * 1024;
  static constexpr int pictureTopPad = 128 * 1024;

  static void usePictureDefaults() {
    const std::lock_guard lock(mutex());
    applyPictures();
  }

  static void hold() {
    const std::lock_guard lock(mutex());
    if (count()++ == 0) {
#ifdef __GLIBC__
      mallopt(M_MMAP_THRESHOLD, videoMmapThreshold);
      mallopt(M_TRIM_THRESHOLD, videoTrimThreshold);
      mallopt(M_TOP_PAD, videoTopPad);
#endif
      threshold() = videoMmapThreshold;
    }
  }

  static void release() {
    const std::lock_guard lock(mutex());
    if (count() == 0 || --count() > 0) return;
    applyPictures();
#ifdef __GLIBC__
    malloc_trim(0);
#endif
  }

  static int holders() {
    const std::lock_guard lock(mutex());
    return count();
  }

  // The mmap threshold last set here, or 0 before any.
  static int mmapThreshold() {
    const std::lock_guard lock(mutex());
    return threshold();
  }

private:
  static void applyPictures() {
#ifdef __GLIBC__
    mallopt(M_MMAP_THRESHOLD, pictureMmapThreshold);
    mallopt(M_TRIM_THRESHOLD, pictureTrimThreshold);
    mallopt(M_TOP_PAD, pictureTopPad);
#endif
    threshold() = pictureMmapThreshold;
  }
  static std::mutex& mutex() {
    static std::mutex value;
    return value;
  }
  static int& count() {
    static int value = 0;
    return value;
  }
  static int& threshold() {
    static int value = 0;
    return value;
  }
};
