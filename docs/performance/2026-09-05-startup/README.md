# Deferred video setup, 5 September 2026

Opening a still image or the library constructed both a `MediaPlayer` and a
`VideoOutput`, even while the viewer was hidden. A diagnostic construction
probe attributed roughly 740 ms to multimedia backend initialization on this
machine. Deferring only the player did not help: the video output's sink also
initializes that backend. Both are now created on the first video request and
retained for later playback. The first video still pays this setup cost.

## Local comparison

Intel Core i7-14700F, 28 logical CPUs, Linux 7.2.3-arch1-2, Qt 6.11.2, Release.
Offscreen NVIDIA RTX 4070 SUPER OpenGL, driver 610.57.04. The software-rendering
request did not select Mesa. Btrfs fixtures, warm filesystem caches, fresh
settings and application cache for every launch. No concurrent builds or tests.
All processes ran inside `tests/run-isolated.sh` with audio devices and session
sockets hidden.

Each mode launched three times before and three times after the change. The
baseline uses the same readiness instrumentation with the original eager
player and output. Each library contains 100 copies of the 160x100 transparent
PNG fixture. Hashes and individual samples are in [before.json](before.json)
and [after.json](after.json).

| Median time from main entry | Eager video setup | Deferred video setup |
| --- | ---: | ---: |
| QML loaded, single-image launch | 810.3 ms | 78.7 ms |
| Requested image frame submitted | 949.9 ms | 259.9 ms |
| QML loaded, library launch | 837.6 ms | 77.1 ms |
| Ready viewport frame submitted | 1181.2 ms | 440.9 ms |

Process RSS after eight seconds ranged from 328.1 to 328.7 MiB before and
250.0 to 251.0 MiB after. This excludes GPU memory. No CPU ticks were observed
in the five-second idle samples; that is not a claim of zero ongoing work.

A separate [photographic fixture check](photo.json) used 100 copies of the
1536x1024 `alpine-dawn.jpg` demo image. One launch per mode reached an image
frame at 258.3 ms and a ready grid at 443.6 ms. This checks that the probe works
with JPEG photography; one sample is not a comparative benchmark.

## Reproduce

Build a Release executable and run from the repository root:

```sh
mkdir -p build/release/benchmark-fixtures
TMPDIR="$PWD/build/release/benchmark-fixtures" \
  bash tests/run-isolated.sh build/release \
  python3 tests/benchmark_startup.py build/release/omaroll
```

The before comparison was built with the original `DetailSheet.qml` from
`3f8703b`, plus the new `imageReady` property, while retaining this PR's tracing
and other readiness checks. The after comparison uses the deferred player and
output. See [metric definitions](../../../tests/README.md#performance).

## Limits and remaining work

These metrics observe decoded QML content followed by a submitted scene-graph
frame. They do not read pixels back or establish when a compositor presents
that frame. Process spawning and dynamic loading before main are excluded.
This replaces the older diagnostic first-frame metric, so the old startup
numbers are not directly comparable.

The first-image predicate rejects errors and checks the requested path. The
grid predicate requires all intersecting cells, including a partial bottom row,
to have decoded thumbnails at full opacity after scanning settles. Empty or
failed libraries do not count as ready.

The small repeated fixture isolates initialization. Warm navigation, large
photographs, mixed media libraries, physical video/audio playback, Wayland
presentation, GPU memory and useful regression budgets still need separate
measurements. These results are not a release performance guarantee.
