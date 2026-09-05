# Validation

Build and run the standard suite:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

The tests create disposable media and settings. The checked-in
[viewer fixtures](fixtures/viewer/README.md) exercise actual decoders without
downloads. UI input goes to the test window, not the live desktop. CTest disables
the PulseAudio connection so automated playback does not use the user's speakers.

## Rendered video

Qt's software scene graph can decode video without displaying it. Run the
OpenGL suite as well. On a headless Arch machine, install `imagemagick`, `mesa`,
`xorg-server-xvfb` and `xorg-xauth`, then run:

```sh
xvfb-run -a bash tests/run-opengl.sh build/release
```

With a working local Mesa display, the script also runs directly. It uses
offscreen Qt and software OpenGL, disables physical audio output, runs the full
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
TMPDIR="$PWD/build/benchmark-fixtures" python3 tests/benchmark_startup.py build/release/omaroll
```

The library benchmark creates distinct 1000x750 PNG files in folders of 500.
Creation is excluded from the timers. Five scans run with warm filesystem
caches, followed by model/proxy construction, a fully evaluated natural sort,
and twenty cold-cache then warm-cache thumbnails. Peak memory belongs to the
benchmark process, which also creates fixtures; it is not application memory.

The startup probe launches the actual executable six times using offscreen Qt
and Mesa software OpenGL. Each launch gets fresh application settings and a
fresh thumbnail cache, with 100 copies of the transparent PNG fixture. It
records receipt of Qt's first completed scene-graph rendering log after the
renderer has done work, then samples idle CPU between seconds three and eight.
This is a diagnostic first-frame measurement. It does not establish when the
requested image or useful grid becomes visible. Zero measured idle CPU means
no CPU ticks were observed during that short interval.

Use `xvfb-run -a` around the startup command on headless CI hosts that need an
X display for Mesa. These measurements are informational, not CI timing gates.
Native GPU rendering, cold disk reads, large photographs, animation and warm
viewer navigation require separate measurements.

A [recorded development baseline](../docs/performance/2026-09-05/README.md)
includes raw results and the measurements still outstanding.
