# Mixed media, 2 October 2026

Gallery photo and document details requested a large video poster even while
that poster was hidden. The source now loads only for an open video. Gallery
and quick-viewer posters also supplied display-scaled dimensions to a provider
that applies scaling itself. Both now supply logical dimensions. Regression
checks cover photo, document, video and closed-sheet states, plus synthetic
1x, 1.5x and 2x scaling.

## Environment and corpus

Intel Core i5-12500, 12 logical CPUs, Linux 6.8.0-146-generic, Release build
with Qt 6.11.2 in an isolated Arch container. Offscreen OpenGL uses Mesa 26.2.3
llvmpipe, LLVM 23.1.1. No desktop audio, session sockets or personal media were
exposed. Builds and tests ran separately from the measurements.

The generator creates 33 seeds: 24 photographic derivatives up to 6000x4000,
alpha PNG, still and animated WebP, GIF, SVG, three MP4 clips and one Matroska
clip. The 10,000-item library uses hardlinks to those seeds. Page caches and
inode metadata are shared; these are generated derivatives rather than
independent camera files. Each application run uses fresh settings and caches.
Filesystem caches are not controlled.

See [commands and metric definitions](../../../tests/README.md#performance).
The generator's manifest records dimensions, bytes, source provenance and
SHA256 hashes. Benchmark commands emit detailed JSON and driver diagnostics.

## Startup and library baseline

Two launches per mode used the actual compiled executable and the large JPEG
inside the 10,000-item library, so the viewer scans that sibling folder too.
Timings start at main entry and measure submitted frames, not presentation.

| Actual executable | Observed range |
| --- | ---: |
| Requested photo decoded, frame submitted | 161.4 ms in both runs |
| Populated gallery viewport submitted | 642.8 to 688.6 ms |
| Viewer peak process RSS during eight seconds | 610.6 to 610.7 MiB |

The gallery's post-start CPU sample was about 103% of one core between seconds
three and eight, while metadata indexing was still active. This is not idle
CPU usage. Software OpenGL also executes graphics work on the CPU.

The separate model benchmark excludes background metadata and rendering.
Scan figures are medians of five scans; sort figures are single observations.

| Corpus | Scan | Natural sort |
| --- | ---: | ---: |
| 10,000 uniform PNGs | 69.7 ms | 10.0 ms |
| 50,000 uniform PNGs | 358.5 ms | 60.9 ms |
| 10,000 mixed items | 78.5 ms | 14.0 ms |

Twenty distinct mixed seeds, sampled across all seven file extensions, took
471.6 ms with a fresh thumbnail cache and 6.9 ms on the cached return. This
checks varied decoding; it is not continuous scrolling or a cold-disk test.

## Navigation and preloading comparison

The interaction harness loads the gallery and viewer from their QML sources,
without the executable's compiled QML cache. Its absolute startup timings
are not comparable with the executable above. It keeps metadata indexing
active, jumps through 12 positions including the library's tail, returns
through the same positions, then steps forward and backward through 32
raster and video seeds. SVG is included only in the gallery measurements.
Stills qualify after their fade finishes. Video frames must match an
independent reference and differ from the other video seeds; pixel conversion
and comparison are included in video timing and resource observations.

Two runs per policy alternated in order. Normal application behavior retains
preloading; the alternative disables it only inside the diagnostic harness.
Each still pass contains 25 observations, including the transparent PNG.

| Within-run median | Neighbor preloading | Diagnostic without preloading |
| --- | ---: | ---: |
| Forward still navigation | 154.7 to 168.0 ms | 155.6 to 159.6 ms |
| Return still navigation | 156.5 to 161.8 ms | 158.7 to 172.1 ms |
| Largest sampled RSS in forward still pass | 902.6 to 905.2 MiB | 618.4 to 622.4 MiB |

There is no consistent latency advantage in these two runs. The forward
memory difference is substantial enough to investigate a bounded preloading
policy, while preserving original-resolution viewing and ordinary navigation.
RSS belongs to the entire process after its gallery phase and includes
retained allocations and software-rendered textures. These endpoint samples
are not allocation peaks; they exclude external decoder processes and
separate GPU memory.

With normal preloading, fresh-cache gallery jumps had within-run medians of
401.1 and 412.7 ms, with individual jumps reaching 2350.2 ms. Cached returns
had medians of 27.8 and 37.9 ms. Visible-thumbnail scheduling and cancellation
are the next gallery targets. The largest heartbeat interval during outward
jumps was about 427 ms; that includes scheduling and rendering delays, not
just GUI-thread blocking.

These are development observations, not timing gates or release guarantees.
Wayland presentation, physical audio, actual fractional-scale displays,
continuous wheel scrolling, independent files and hardware GPU memory need
separate coverage.
