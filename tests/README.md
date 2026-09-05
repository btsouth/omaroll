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
UI suite and renders narrow grid, video and OCR views. The video-track test
requires actual colored pixels in the rendered video area. Inspect the PNGs in
`build/release/opengl-renders/` for layout changes.

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

## Performance

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

`OMAROLL_STARTUP_TRACE=1` enables diagnostic JSON lines on stderr. Timings start
at entry to `main`, excluding process spawning and dynamic loading before main:

- `application`, `theme`, `services`, `qml`: cumulative setup milestones.
  `services` is recorded after image providers and context properties are
  registered, immediately before QML loading.
- `first_frame`: first scene-graph frame submitted.
- `image_frame`: frame submitted after the requested still image reports a
  successful decode and has nonzero display dimensions.
- `grid_frame`: frame submitted after scanning settles and every cell
  intersecting the viewport has a decoded thumbnail at full opacity. An empty
  library, missing delegate, failed decode or fading thumbnail does not qualify.

Readiness is sampled on the GUI thread before scene synchronization, latched
at synchronization, then reported at Qt's
[afterFrameEnd](https://doc.qt.io/qt-6/qquickwindow.html#afterFrameEnd) signal.
These are content-ready submitted frames, not pixel readbacks or proof of
presentation by Hyprland. The UI tests separately exercise failed images,
thumbnail fading and rendered media. Normal launches do not enable tracing.

The probe fails if required milestones are absent within its eight-second
observation window. CI checks that evidence arrives and retains the JSON; it
sets no speed threshold. Idle CPU is sampled between seconds three and eight.
Zero means no CPU ticks were observed in that interval. Do not run benchmarks
concurrently with builds or other tests.

Use `xvfb-run -a` around the startup command on headless CI hosts that need an
X display for Mesa. These measurements are informational, not CI timing gates.
Wayland presentation, cold disk reads, large photographs, animation and warm
viewer navigation require separate measurements.

The [startup follow-up](../docs/performance/2026-09-05-startup/README.md) records
content-ready timing and the deferred-video comparison.

A [recorded development baseline](../docs/performance/2026-09-05/README.md)
includes raw results and the measurements still outstanding.
