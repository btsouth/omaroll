# Bounded media work, 3 October 2026

Each quick viewer now shares a 64 MiB decoded-pixel estimate across its two
neighboring images. The displayed image keeps its original resolution. Header
probes run off the GUI thread, with one probe in flight and navigation replacing
the pending pair. Decoder depth determines the estimate, with a minimum of
four bytes per pixel and sixteen for an unknown format. Generation and file
version checks reject stale approvals. Wrapped duplicates, animations, video
and unknown dimensions are excluded.

This is an admission budget for neighboring images, not an RSS cap. Decoder
scratch space, graphics allocations and Qt's retained cache are outside it.

Thumbnail cancellation is checked between decode stages and before cache
publication. Video helper waits check cancellation every 50 ms and kill and
reap obsolete helpers. Canceled work skips fallback decodes and failure caching.
Shutdown drains queued responses with a shared stop flag, so each response can
finish on its own thread. Individual image and PDF decodes remain indivisible;
a canceled PDF request can still wait for the existing 12-second deadline.

## Comparison

The environment, 10,000-item generated corpus and metric definitions match the
[mixed-media baseline](../2026-10-02-mixed-media/README.md). Each run has fresh
application settings and caches; filesystem caches are uncontrolled. Builds
and tests ran separately. The interaction harness uses source QML and llvmpipe
OpenGL, with metadata indexing active. These are submitted frames, not physical
Wayland presentation. Two normal runs were recorded after the change.

| Observation | Previous two runs | Bounded work, two runs |
| --- | ---: | ---: |
| Forward still navigation, within-run median | 154.7 to 168.0 ms | 34.8 to 37.1 ms |
| Return still navigation, within-run median | 156.5 to 161.8 ms | 18.4 to 20.3 ms |
| Largest sampled RSS in forward still pass | 902.6 to 905.2 MiB | 673.5 to 700.0 MiB |
| Largest sampled RSS in return still pass | 1029.1 to 1050.8 MiB | 840.8 to 984.8 MiB |
| Fresh-cache gallery jumps, within-run median | 401.1 to 412.7 ms | 381.3 to 409.6 ms |
| Cached gallery returns, within-run median | 27.8 to 37.9 ms | 22.8 to 37.6 ms |

Each normal run completed all 89 operations. Each still pass has 25 images;
readiness includes the full fade when a decode is needed. The viewer follows
the gallery in the same process, so RSS includes retained allocations. These
endpoint samples are not allocation peaks and exclude external decoder
processes and dedicated GPU memory. The forward memory reduction was more
consistent than the return pass. Gallery timings do not show a clear gain.

The actual compiled executable was also launched twice per mode with the same
large JPEG inside the 10,000-item library. Timings start at main entry. Its
startup figures are separate from the source-QML interaction measurements.

| Actual executable | Previous two runs | Bounded work, two runs |
| --- | ---: | ---: |
| Requested photo decoded, frame submitted | 161.4 ms | 157.5 to 157.8 ms |
| Populated gallery viewport submitted | 642.8 to 688.6 ms | 661.9 to 663.8 ms |
| Viewer peak process RSS during eight seconds | 610.6 to 610.7 MiB | 519.4 to 519.7 MiB |

Gallery post-start CPU remained about 105% of one core with background metadata
indexing active. No idle-CPU or gallery startup improvement is claimed.

An optional `--burst-jumps 24` issues gallery jumps 16 ms apart, deliberately
without settling intermediate viewports, then waits for the final viewport.
Two baseline runs took 2297.9 and 3617.0 ms; one final run took 3240.9 ms and
completed all 90 operations. Timer delivery includes GUI and rendering delays.
This establishes a repeatable stress case, not a measured scrolling improvement.

## Regression coverage

Tests cover combined JPEG/PNG admission, 64-bit image depth, budget boundaries,
stale navigation and replacement, sequence refreshes, wrapped duplicates and
original-resolution cache reuse. Thumbnail tests hold helper processes in
every pool slot, cancel running and queued requests, verify immediate retries,
and check shutdown, signal affinity and deferred deletion. Genuine decode
failures still use the negative cache.

Visible-thumbnail priorities, continuous wheel scrolling, independent camera
files and hardware presentation remain separate follow-ups. These observations
are development comparisons, not CI timing gates or release guarantees.
