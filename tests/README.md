# Validation

Build and run the standard suite:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
bash tests/run-isolated.sh build/release
```

The tests create disposable media and settings. The checked-in
[viewer fixtures](fixtures/viewer/README.md) exercise actual decoders without
downloads. UI input goes to the test window, not the live desktop. Local checks use
`bubblewrap` to hide session audio sockets and ALSA devices. Tests and headless
renders also force a disconnected PulseAudio backend, block native PipeWire,
and refuse playback when Qt still exposes audio outputs. `PULSE_SERVER` alone
does not block Qt's native PipeWire backend.

## Rendered video

Qt's software scene graph can decode video without displaying it. Run the
OpenGL suite as well. In a disposable headless Arch CI container, install `imagemagick`, `mesa`,
`xorg-server-xvfb` and `xorg-xauth`, then run:

```sh
xvfb-run -a bash tests/run-opengl.sh build/release
```

On the development desktop, always run it inside the local audio sandbox:

```sh
bash tests/run-isolated.sh build/release bash tests/run-opengl.sh build/release
```

It uses
offscreen Qt and requests software OpenGL (some drivers use hardware instead),
uses the disconnected audio backend, runs the full
UI and viewer suites and renders the view matrix below. The video-track test requires
actual colored pixels in the rendered video area.

The render matrix covers every view in the dark palette, the views that carry
the most chrome in the light palette, and grid, detail, video, document, OCR
the correction sheet and the viewer in the smallest window the app allows. The viewer views open a
demo file the way a file manager would, with the controls held up. Each render runs with a disposable
profile pointed at a theme fixture under `tests/fixtures/themes/`, so the
palette is the fixture's rather than whatever this machine happens to use, and
the PNGs are comparable between runs. Add a view to the lists at the end of
`tests/run-opengl.sh` when it gains a surface worth looking at in a second
palette; each render costs about twelve seconds of deliberate settling.
Inspect the PNGs in `build/release/opengl-renders/` for layout changes.

CI runs both suites. Release validation additionally installs the package,
renders its video view under Xvfb, and checks upgrade, reinstall and removal.

## Desktop acceptance

Headless passes do not establish physical audio or live desktop integration.
For a release candidate, record version, GPU, Omarchy version, display scales
and results for:

- Open With from the file manager, including spaces and Unicode paths.
- Image/text clipboard transfer and drag/drop to another application.
- Multiple monitors, fractional scaling, fullscreen and minimize/restore.
- Theme changes with the viewer open and media opacity.
- Physical audio, track changes, long-file seeking and subtitle readability.
- Unavailable external tools, disconnected source folders and failed actions.

Use the fixtures and a disposable folder for operations that modify files.
Keep untested environments explicit in the release report.

### Functional acceptance checklist

Use disposable media for operations that change files. Check the library and
quick viewer separately, including both themes and narrow windows.

1. Launch `omaroll --demo` and verify the grid, folder filters, search, sort,
   selection, and Settings at both tiled and floating window sizes.
2. Open a real image from the file manager. Verify previous and next stay in its
   folder, then test fit, actual size, deep zoom, pan, rotate, fullscreen,
   animation pause and `F5` slideshow. Press Enter to open the library preview
   and check its horizontal and vertical flips.
3. Open a real video. Verify play and pause, sound, volume, seeking, playback
   speed, audio tracks, subtitles, technical details, Save current frame,
   double-click fullscreen, and the transition to the next item in a slideshow.
4. Create an album, rename and move one member inside a watched folder, and
   verify it remains in the album. Move it outside the library and verify it is
   shown as unavailable instead of being matched to another file.
5. Put an exact copy of a disposable image in another watched folder. Open
   Browse, choose Exact duplicates, keep one selected copy, and verify only the
   other byte-for-byte copies move to Trash after confirmation.
6. Resize or recompress a disposable image, open Similar pictures, and verify
   the pair is grouped without either file being modified.
7. Add a tag and saved view, restart Omaroll, and verify both persist. Rename a
   tagged file inside Omaroll and verify its tag follows it.
8. Open a multipage PDF from the file manager and verify thumbnails, page
   navigation, continuous scrolling, fit page, text search, word selection and
   copying. Check details, rename, organize, send and Trash actions.
9. Switch Omarchy themes while Omaroll is open and confirm the chrome updates
   without changing the media colors.
10. Move a disposable file to Trash and restore it from the desktop Trash.

## Performance

On Linux, Omaroll defaults Qt FFmpeg decoding to `cuda,vaapi` when the NVIDIA
driver marker and loadable `libnvcuvid.so` pass Qt's CUDA prerequisites,
otherwise `vaapi`. Encoding defaults to
an empty hardware list (`,`). Setting both avoids Qt's general hardware-device
scan, including recording devices a viewer does not need. Qt retains software
decoding when the listed devices or a file's codec are unavailable. Existing
`QT_FFMPEG_DECODING_HW_DEVICE_TYPES` and `QT_FFMPEG_ENCODING_HW_DEVICE_TYPES`
values, including explicit empty values, take precedence. Apps launched through
Omaroll's actions receive the caller's original hardware settings.
The driver prerequisites are checked once, when the first player is created.

These are [documented private Qt controls](https://doc.qt.io/qt-6/advanced-ffmpeg-configuration.html),
so verify device selection and rendered video when upgrading Qt. Other operating
systems retain their Qt defaults. Isolated software fallback does not establish
accelerated playback on NVIDIA, Intel or AMD hardware.

The optional benchmarks keep their files, settings and cache in disposable
directories. Select the filesystem deliberately: `/tmp` may be RAM-backed.
Run a Release build on an otherwise quiet machine and retain the JSON output.

```sh
cmake --build build/release --target omaroll_benchmark_library
mkdir -p build/benchmark-fixtures
TMPDIR="$PWD/build/benchmark-fixtures" build/release/omaroll_benchmark_library 10000
TMPDIR="$PWD/build/benchmark-fixtures" build/release/omaroll_benchmark_library 50000
mkdir -p build/release/benchmark-fixtures
TMPDIR="$PWD/build/release/benchmark-fixtures" bash tests/run-isolated.sh build/release \
  python3 tests/benchmark_startup.py build/release/omaroll
```

The library benchmark creates distinct 1000x750 PNG files in folders of 500.
Creation is excluded from the timers. Five scans run with warm filesystem
caches, followed by model/proxy construction, a fully evaluated natural sort,
and twenty cold-cache then warm-cache thumbnails. Peak memory belongs to the
benchmark process, which also creates fixtures; it is not application memory.

The startup probe launches the actual executable six times using offscreen Qt
and OpenGL, recording the actual graphics driver. Each launch gets fresh
settings and a fresh thumbnail cache, with 100 copies of the transparent PNG.
Use `--fixture resources/demo/alpine-dawn.jpg` to measure a photographic image,
`--files N` to change the library size, and `--runs N` for repeated samples.
Fixture creation is excluded. The output records the fixture hash and size.
`observed_process_stages_ms` also records when the probe receives each milestone,
measured from process launch. Those observations include spawning and dynamic
loading, plus delivery of the trace; they do not measure compositor presentation.

`OMAROLL_STARTUP_TRACE=1` enables diagnostic JSON lines on stderr. Timings start
at entry to `main`, excluding process spawning and dynamic loading before main:

- `application`, `theme`, `services`, `qml`: cumulative setup milestones.
  `services` is recorded after image providers and context properties are
  registered, immediately before QML loading.
- `first_frame`: first scene-graph frame submitted.
- `image_frame`: frame submitted after the requested still image reports a
  successful decode and has nonzero display dimensions. For videos, the
  existing milestone uses the player's `hasVideo` track flag; it does not
  establish decoded pixels or presentation.
- `poster_frame`: frame submitted with the quick viewer's video thumbnail
  ready and visible, before the player takes over.
- `video_decoded_frame`: first valid video frame observed at the quick viewer's
  sink. Observation happens on the GUI thread and can include a frame that
  arrived before the sink was attached to tracing. This is not a submitted-frame
  milestone or proof of physical presentation.
- `grid_frame`: frame submitted after scanning settles and every cell
  intersecting the viewport has a decoded thumbnail at full opacity. An empty
  library, missing delegate, failed decode or fading thumbnail does not qualify.

Readiness is sampled on the GUI thread before scene synchronization, latched
at synchronization, then reported at Qt's
[afterFrameEnd](https://doc.qt.io/qt-6/qquickwindow.html#afterFrameEnd) signal.
These are content-ready submitted frames, not pixel readbacks or proof of
presentation by Hyprland. The UI tests separately exercise failed images,
thumbnail fading and rendered media. Normal launches do not enable tracing.

The probe fails with recent application diagnostics if required milestones are
absent within its eight-second observation window. CI checks that evidence
arrives and retains the JSON; it sets no speed threshold. Post-start CPU is sampled
between seconds three and eight. Background indexing may still be running; this
is not a guarantee that the application is idle.
Zero means no CPU ticks were observed in that interval. Do not run benchmarks
concurrently with builds or other tests.

Use `xvfb-run -a` around the startup command on headless CI hosts that need an
X display for Mesa. These measurements are informational, not CI timing gates.
Wayland presentation and cold disk reads require separate measurements.

For mixed media, generate a separate corpus before measuring (requires
ImageMagick 7 and FFmpeg tools). Use a new or empty directory:

```sh
python3 tests/make-performance-fixtures.py build/release/performance-fixtures --files 10000
cmake --build build/release --target omaroll_benchmark_interaction
bash tests/run-isolated.sh build/release build/release/omaroll_benchmark_library \
  --library "$PWD/build/release/performance-fixtures/library"
bash tests/run-isolated.sh build/release python3 tests/benchmark_startup.py \
  build/release/omaroll --library "$PWD/build/release/performance-fixtures/library" \
  --fixture "$PWD/build/release/performance-fixtures/library/media-1-seed-01-generated-derived-photo-6000x4000-alpine-dawn.jpg"
bash tests/run-isolated.sh build/release env QT_QUICK_BACKEND=rhi \
  QSG_RHI_BACKEND=opengl LIBGL_ALWAYS_SOFTWARE=1 QSG_INFO=1 \
  build/release/omaroll_benchmark_interaction \
  --library "$PWD/build/release/performance-fixtures" --steps 12
```

The corpus includes 24 generated photo derivatives up to 24 megapixels, alpha
PNG, still and animated WebP, GIF, SVG, MP4 and Matroska. Its manifest records
source provenance, dimensions and hashes. Library entries are hardlinks to
33 seeds: they share inode metadata and filesystem caches. This measures
cardinality and varied decoding, not a collection of independent camera files
or cold disk performance. The interaction probe loads the gallery and viewer
from their QML sources, without the executable's compiled QML cache, in one
process, keeps production metadata indexing active, and measures
submitted frames after fresh-cache jumps across the library, cached returns,
and forward/backward stepping through raster and video seeds. SVG is included
in gallery coverage and explicitly excluded from viewer measurements. A video
requires a valid current-source frame whose pixels match an independently
prepared FFmpeg reference and differ from the other video seeds. This checks
source identity with a tolerant 32x18 RGB comparison; indistinguishable video
fixtures cannot qualify. Reference preparation is outside operation timers,
but frame conversion and comparison are included. Still readiness also waits
for the loader's fade to finish. Decoder errors, QML errors and 30-second
operation timeouts fail the run. The default gallery passes use deliberate jumps.
Retain stderr for actual `QSG_INFO` driver details beside JSON.

Add `--burst-jumps 24` to issue gallery jumps 16 ms apart before the normal
passes. The extra sample measures from the first input until the final viewport
at the end of the library is ready. Intermediate viewports deliberately do not
settle. Timer delivery can be delayed by rendering or other GUI work, so this
measures the complete burst and final settling, not a fixed-duration input trace.

Alternatively, add `--wheel-steps 120` for phased touchpad input through Qt's
normal event delivery. It requests 40-pixel updates every 16 ms, records actual
travel and delivery duration, and waits for the final viewport to settle.
At least 80% of the expected travel and a nonzero submitted-frame count are
required. The sample counts submitted frames with a fully faded, ready
viewport during input. That readiness snapshot is not a pixel-coverage
measurement or physical presentation evidence. GUI work can delay timer
delivery, so compare actual duration as well as ready-frame counts. Wheel and
burst modes are mutually exclusive.

The 10 ms heartbeat reports the largest interval between GUI callbacks, not
pure blocking time. RSS samples belong to the whole benchmark process; its
viewer phase follows the gallery phase and includes retained allocations.
These samples exclude external decoder processes and dedicated GPU memory;
software OpenGL textures can contribute to process RSS.
`--without-prefetch` disables neighboring-image loads only in this diagnostic
process for a controlled comparison. It changes no installed preference or
normal application behavior.

Inputs remain read-only; application profiles and thumbnail caches are
disposable. Do not point these probes at personal media.

The [startup follow-up](../docs/performance/2026-09-05-startup/README.md) records
content-ready timing and the deferred-video comparison.
The [bounded-work follow-up](../docs/performance/2026-10-03-bounded-media/README.md)
records mixed-media navigation and rapid-jump comparisons.
The [gallery loading follow-up](../docs/performance/2026-10-03-gallery-loading/README.md)
records continuous input and the scheduling policy that was discarded.

A [recorded development baseline](../docs/performance/2026-09-05/README.md)
includes raw results and the measurements still outstanding.
