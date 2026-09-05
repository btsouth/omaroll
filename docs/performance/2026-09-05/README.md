# Development baseline, 5 September 2026

Measured during multi-file opening development after 1.4.0. These are local
engineering measurements, not release performance guarantees.

The [startup follow-up](../2026-09-05-startup/README.md) adds content-ready
frame measurements and compares eager and deferred video initialization.

Environment: Intel Core i7-14700F, 28 logical CPUs, 31.1 GiB RAM,
Linux 7.2.3-arch1-2, Qt 6.11.2, Release build. Disposable fixtures were placed
on the project's Btrfs filesystem, not the RAM-backed `/tmp`. Filesystem caches
were warm. No cache flushing or changes to the running desktop were made.

Commands and precise metric definitions are in [tests/README.md](../../../tests/README.md#performance).
Raw results are preserved alongside this report.

| Library operation | 10,000 files | 50,000 files |
| --- | ---: | ---: |
| Discovery, median of five scans | 51.2 ms | 261.9 ms |
| Model and proxy ready | 65.9 ms | 297.4 ms |
| Natural filename sort | 12.2 ms | 62.4 ms |
| Twenty thumbnails, empty thumbnail cache | 82.7 ms | 84.6 ms |
| Same twenty thumbnails, populated cache | 3.4 ms | 3.4 ms |
| Benchmark process peak RSS | 103.2 MiB | 158.5 MiB |

The synthetic library contains distinct 1000x750 solid-color PNG files in
folders of 500. These results do not measure high-resolution photography,
video thumbnails, remote drives, OCR or a complete rendered library.

| Actual executable, offscreen NVIDIA OpenGL | Median of three launches |
| --- | ---: |
| First scene-graph frame, single-image launch | 1073.1 ms |
| First scene-graph frame, library launch | 1110.7 ms |

Each launch used 100 small PNG fixtures, fresh settings and an empty thumbnail
cache. Observed process RSS after eight seconds ranged from 328.1 to 328.8 MiB.
Between seconds three and eight, CPU use was 0 to 0.20% of one core. Zero means
no CPU ticks were observed in that short sample.

The startup samples were repeated inside the local audio sandbox after fixing
Qt audio isolation. Qt reports an NVIDIA GeForce RTX 4070 SUPER, driver
610.57.04. The software-rendering environment request did not select Mesa on
this host; the probe now records the actual driver in every sample.

The first scene-graph frame can precede the requested image or useful grid.
It is not an image-ready measurement or a substitute for Hyprland presentation
measurements. The thumbnail cache's default disk
ceiling is 256 MiB; this is separate from process memory.

Next measurements: first requested image actually visible, first useful grid,
warm viewer navigation, larger real photographs, Wayland presentation and GPU memory.
Keep the performance issue open until those measurements and repeatable
regression budgets are recorded. Investigate startup before claiming faster
opening or setting timing gates from the scanner numbers.
