# Performance

Current numbers for the Release build, with mpv and imv measured the same way
alongside. The dated folders here hold the investigations behind each change.

## Current numbers, 9 November 2026

Medians, in milliseconds from process start unless a row says otherwise.

| | Omaroll | mpv | imv |
| --- | ---: | ---: | ---: |
| Window mapped, 1080p video | 237 | 257 | |
| First video frame decoded | 365 | | |
| First video frame shown | 404 | 257 | |
| Window mapped, 24 MP JPEG | 194 | | 98 |
| First image frame | 246 | | |
| 10,000-file grid, thumbnails cached | 699 | | |
| 10,000-file grid, fresh thumbnails | 870 | | |
| 50,000-file grid, thumbnails cached | 1,173 | | |
| 50,000-file grid, fresh thumbnails | 1,274 | | |
| 10,000-file grid offscreen, fresh thumbnails (llvmpipe) | 2,004 | | |
| 4K60 H.264 playback, % of one core | 21.5 | 3.5 | |
| 4K60 10-bit HEVC playback, % of one core | 30.2 | 4.2 | |
| Indexing 10,000 files, seconds | 12.5 | | |
| Window thread while indexing, % of one core (GPU desktop) | 3.8 | | |

The grid rows are from `main()`, which starts about 25 ms after the process.
Playback cost is explained in [playback cost](2026-11-09-playback-cost/README.md).

### How it was measured

- Intel Core i7-14700F, NVIDIA RTX 4070 SUPER (driver 615.78), Qt 6.12.0,
  FFmpeg 9.0.2. A headless Hyprland at 1920x1080@60 in omabox, so every
  window is a real Wayland surface. OpenGL ES through EGL on the NVIDIA card,
  CUDA decoding. A host load average of 2 to 4.
- Each launch has a fresh Omaroll profile (settings, thumbnails, metadata
  index). The fontconfig and NVIDIA shader caches are warm, as on any desktop
  that has run a Qt app before. Launches of each program are interleaved,
  6 to 10 of each.
- "Window mapped" is Hyprland's `openwindow` event, read from its event
  socket. "Shown" for Omaroll is the first frame submitted with a decoded
  picture (`video_frame`); for mpv it is its own "first video frame after
  restart shown" log line, timed on arrival. mpv ran with
  `--no-config --hwdec=auto --vo=gpu --gpu-api=opengl`; its default Vulkan
  output took about 800 ms here. imv has no frame log, so only its window is
  timed.
- The video is a 1080p30 H.264 Matroska file with two AAC tracks and SRT
  subtitles; the picture a 24 MP JPEG alone in its folder. The libraries are
  `tests/make-performance-fixtures.py` output, mostly large JPEGs with some
  videos, GIFs and SVGs. Playback is `tests/benchmark_playback.py --gpu`;
  indexing is `tests/benchmark_indexing.py --gpu`.
- Frames are submitted or decoded, not counted at the display. Audio went to
  the box's null sink.

### A fresh profile is not a fresh launch

mpv's `--msg-time` clock starts 60 to 80 ms after the process does, while it
loads its libraries, so its log times are not times from launch.

A fresh `XDG_CACHE_HOME` also empties fontconfig's and the GPU driver's
caches. This system keeps no system-wide fontconfig cache, so the first
program a user runs pays for building it. Measured that way, Omaroll waited
about 160 ms in its theme for fonts and 60 ms more for shaders:

| Fresh caches too | Before | This change |
| --- | ---: | ---: |
| First video frame decoded, from `main()` | 629 | 610 |
| First image frame, from `main()` | 409 | 399 |

## Startup, stage by stage

From `main()`, with `OMAROLL_STARTUP_TRACE=1` (stages are described in
[tests/README.md](../../tests/README.md)). Before is main at `a01b566`.

| Video in the quick viewer | Before | After |
| --- | ---: | ---: |
| `theme` | 23 | 24 |
| `qml` | 122 | 131 |
| `scenegraph` | | 210 |
| `first_frame` | 239 | 235 |
| `media_loaded` | | 242 |
| `image_frame` (track flag) | 322 | 304 |
| `video_decoded_frame` | 367 | 351 |
| `video_frame` | | 381 |

| Picture in the quick viewer | Before | After |
| --- | ---: | ---: |
| `theme` | 21 | 22 |
| `qml` | 104 | 105 |
| `first_frame` | 194 | 191 |
| `image_frame` | 220 | 220 |

What changed:

- A direct video launch checks the CUDA driver, starts it and loads Qt's
  FFmpeg backend on a worker while the window loads (`src/app/VideoWarmup.h`).
  Creating the first CUDA decoder on the window thread fell from about 82 to
  65 ms, and the backend is no longer loaded there. The worker's library loads
  briefly hold the dynamic loader's lock, which costs QML loading about 10 ms.
- The thumbnail pool runs up to eight threads rather than four, so a first
  look at a big folder fills its tiles sooner.

The rest of the time before the first frame is spent in Qt and the driver:
loading QML, a temporary OpenGL context Qt creates on the window thread to
choose a surface format (about 40 ms with NVIDIA), and the render thread's own
context. Nothing before the theme is worth moving: the single-instance check
takes under a millisecond, the icon lookup about 3, hyprctl about 7 and the
font lookup about 10 with a warm cache.

## Why video trails mpv

The first video frame is a chain on the window thread. Qt opens the file in
about 10 ms, but the window thread only hears of it after the window's first
frame, because Qt blocks that thread while the render thread creates its
context and draws (about 100 ms). Qt then creates the CUDA decoder on the
window thread (about 65 ms), and only then does decoding start.

An experiment that started the player while the window was still hidden, and
showed the window once the file was open, put the decoded picture in the
window's first frame at 354 ms from launch, 106 ms behind mpv. The window
mapped 110 ms later, at 347 ms. That needs a change in `qml/Viewer.qml`, whose
player only gets a source once the window is visible, and a decision on
whether a later window with the picture beats an earlier one without it.

## Budgets in CI

CI runs on software rendering and decoding, several times slower than the
table above, so these catch a stage that starts waiting on something rather
than a few milliseconds. Each is a median of three launches; aarch64 scales
the startup budgets by three.

| Check | Budget | On the devbox container |
| --- | ---: | ---: |
| Picture: `qml` / `image_frame` | 1,500 / 3,000 ms | 190 / 570 ms |
| Video: `qml` / `video_frame` | 2,000 / 8,000 ms | 450 / 2,600 ms |
| 100-file library: `qml` / `grid_frame` | 3,000 / 9,000 ms | 840 / 3,100 ms |
| 4K30 software playback | 350% of a core, 60,000 faults/s | 60%, 6,000 to 20,000 |
| Window thread while indexing 10,000 files (nightly) | 10% of a core | 2.3% (offscreen) |

The fault budget is per second, so a runner that decodes fewer frames also
faults less. The per-frame guard is `omaroll_tests`, which sends 4K frames
through the presentation and fails above 100 faults a frame. It skips when frames are not
mapped even with the picture settings, as on ARM, since then there is nothing to compare.

Software-decoded frames come from FFmpeg's buffer pool, so the playback check
cannot see the per-frame copies of hardware frames that #159 fixed.
`omaroll_tests` covers that path instead: it hands 4K NV12 frames to the
presentation from a decoder thread and fails above 100 page faults a frame.
A mapped frame costs about 3,000.
