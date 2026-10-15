# Video startup, 14 October 2026

Opening a video in the quick viewer waited for its thumbnail to be drawn before
creating the player, with a 250 ms fallback. That ordering dates from when
creating the multimedia backend blocked the window for about 740 ms. With
explicit hardware decoder lists ([#83](https://github.com/btsouth/omaroll/pull/83)) and Qt 6.12, creating the player now
takes about 17 ms on the GUI thread, so the wait only delayed playback. The
player now starts on the next turn after the file is opened, and the thumbnail
and the first decoded frame load side by side.

## Direct launch

Intel Core i7-14700F, NVIDIA RTX 4070 SUPER (driver 615.78), Qt 6.12.0 Release
builds of main (`81a79fc`) and this change, offscreen OpenGL on the graphics
card inside `tests/run-isolated.sh`. A fresh profile per launch, a generated
1920x1080 30 fps H.264 clip with AAC audio, six interleaved launches per build.
Times are from main entry, using `OMAROLL_STARTUP_TRACE=1`.

| Median | main | This change |
| --- | ---: | ---: |
| QML loaded | 144 ms | 148 ms |
| First decoded video frame | 469 ms (448 to 584) | 359 ms (335 to 442) |

Starting the player directly in the open handler, rather than on the next turn,
reached the frame at 351 ms but delayed QML loading to 165 ms.

## Hardware and software decoding

Measured with a small standalone Qt player in the same sandbox, timing from
setting the source to the first valid frame at the video sink:

| | Software | CUDA | VA-API (NVIDIA driver) |
| --- | ---: | ---: | ---: |
| First video in the process | 45 ms | 125 ms | 270 ms |
| Next video | 35 ms | 155 ms | 290 ms |

Qt creates and destroys the hardware device for every video and reads the
decoder list once per process, so this cost cannot be paid once or chosen per
file. In the viewer, stepping to the next video took about 80 ms with software
decoding and 170 ms with CUDA.

Software decoding costs CPU instead. Decoding without presentation, as a share
of one core:

| Clip | Software | CUDA |
| --- | ---: | ---: |
| 1080p30 H.264, grainy, 16 Mbit/s | 77% | 11% |
| 2160p60 HEVC 10-bit, grainy, 50 Mbit/s | 317% | 13% |

Hardware decoding stays the default. A laptop decoding 4K HEVC in software
would spend several cores and its battery to save about a tenth of a second.

## Limits

Generated clips, one machine, offscreen rendering. No Wayland presentation,
physical audio or Intel and AMD VA-API timings were measured. Audio output was
blocked in the sandbox, so connecting to PipeWire is not included.
