# Playback cost, 9 November 2026

Playing a 4K60 video used 37% of a core for H.264 and 74% for 10-bit HEVC,
almost all of it on Qt Quick's render thread. A plain Qt Quick player
(`MediaPlayer` and `VideoOutput`, nothing else) used about half that on the
same files.

The difference was ours. Since 1.16.1, Omaroll fixes glibc's mmap threshold at
4 MB so that decoded pictures go straight back to the system
([#124](https://github.com/btsouth/omaroll/pull/124)). With hardware decoding,
Qt copies every frame into a new buffer, 12 MB for 8-bit 4K and 25 MB for
10-bit. Above the threshold each buffer was mapped, faulted in page by page
and unmapped again, 60 times a second: 100,000 to 350,000 page faults a
second. Giving a plain Qt player the same 4 MB threshold raised it to
Omaroll's numbers.

Omaroll now raises the threshold and keeps its heaps while any video shows
frames, then restores the picture setting and trims the heap when the last
video goes. Pictures are freed as before, and closing a playing preview
returned the process to 586 MB rather than 714 MB.

## Results

Intel Core i7-14700F, NVIDIA RTX 4070 SUPER (driver 615.78), Qt 6.12.0,
FFmpeg 9.0.2, Release builds of main (`6e144ed`) and this change. Each run is
a fresh profile in a headless Hyprland desktop (omabox, 1920x1080 at 60 Hz),
with the viewer tiled at 1914x1045 and Omaroll's default CUDA decoding. CPU is
the whole process, as a share of one core, over 6 seconds starting 4 seconds
after launch. Clips are 20-second `testsrc2` encodes at 40 Mbit/s.

| Median of 5 runs | Before | After | Plain Qt player | mpv `--hwdec=auto` |
| --- | ---: | ---: | ---: | ---: |
| 4K60 H.264, viewer | 37.3% | 21.8% | about 21% | 7.4% |
| 4K60 HEVC 10-bit, viewer | 73.5% | 40.7% | about 38% | 6.4% |
| 4K60 HEVC 10-bit, library preview (3 runs) | 76.6% | 35.6% | | |
| Page faults a second, HEVC 10-bit | 324,000 to 350,000 | about 1,600 | about 1,300 | |

The CI check (`tests/benchmark_playback.py`, software decoding and llvmpipe,
4K30 H.264) is about the same before and after, 74 to 84% of a core on a
Ryzen 7 9700X: software-decoded frames come from FFmpeg's own buffer pool, so
they never hit the threshold. The same script with `--gpu`, run in omabox on
the NVIDIA card, measured 67.7% and 367,000 faults a second before, and 33.0%
and 2,700 after.

## What remains

The rest of the cost is Qt Multimedia's. Qt 6.12 has a zero-copy path only for
VA-API frames, by importing them as DMA-BUF textures. CUDA frames are copied
to system memory on the render thread and uploaded again, about 750 MB a
second at 4K60 8-bit and twice that at 10-bit. mpv keeps the frame on the
graphics card and uses 6 to 8% of a core.

The alternatives measured worse on this machine:

| Omaroll 1.18.0, 4K60 | H.264 | HEVC 10-bit |
| --- | ---: | ---: |
| CUDA (default) | 46% | 77% |
| VA-API through `nvidia-vaapi-driver` | 74% | 102% |
| Software decoding | 78% | 143% |

With the NVIDIA VA-API driver, `vaExportSurfaceHandle` failed for every frame,
so Qt fell back to reading the frame back. Intel and AMD VA-API, where Qt's
DMA-BUF import should apply, were not measured.

## Limits

Generated clips, one machine and one GPU, a headless compositor with no
physical display, and no audio output. The window was 1920x1080; a 4K display
draws more pixels, but the copies above do not depend on the window size.
Numbers moved by up to ten points between runs while other GPU work was
running, so compare runs made together.
