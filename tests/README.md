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
OpenGL suite as well. On a headless Arch machine, install `mesa`,
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
