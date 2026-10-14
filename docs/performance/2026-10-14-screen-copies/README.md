# Screen-sized copies, 14 October 2026

The quick viewer decoded every still at full resolution and kept that decode
alongside a mipmapped texture, even while the picture was fitted to a window
smaller than a quarter of it. A heap profile of one 12-megapixel photo put
almost all of the viewer's memory in those pixels; QML was under 10 MiB.

Stills whose longest side is more than a quarter past the largest screen's now
open from the copy the very large picture path already made, at the screen's
size, and load full resolution when zoomed past it. Neighbours are preloaded as
the same copies, so both fit the shared preload budget that one whole decode of
such a photo used to fill. glibc's large-allocation threshold is also fixed at
4 MiB, so freed decodes are returned to the system instead of being kept in its
heaps after the first large free.

## Isolated desktop

Intel Core i7-14700F, Qt 6.11.2 Release builds of main (`d843f96`) and this
change, run in an isolated Omarchy desktop at 2560x1440 with a tiled window.
That desktop renders in software (Mesa llvmpipe), so window buffers and textures
count as process memory here; on a graphics card they are video memory instead.
Fixtures are generated: 4032x3024 JPEGs at quality 90, alone or twelve in a
folder.

| Process RSS | main | This change |
| --- | ---: | ---: |
| One photo, after settling | 425 MiB | 347 MiB |
| Twelve photos, after stepping through all | 537 to 539 MiB | 389 to 415 MiB |
| Twelve photos, peak while stepping | 564 to 630 MiB | 434 to 447 MiB |

A 1920x1080 PNG, smaller than the screen, is shown whole as before; its
measurements are within run-to-run variation.

## Startup benchmark

`tests/benchmark_startup.py` with five runs per build, offscreen Mesa llvmpipe in
the CI container, twelve copies of the same photo. Its screen is 800x600, so the
copy is 2048 pixels long.

| Single-image launch, median | main | This change |
| --- | ---: | ---: |
| Requested image frame submitted | 195 ms | 187 ms |
| Process RSS after eight seconds | 364 MiB | 250 MiB |
| Peak process RSS | 388 MiB | 259 MiB |

## Picture quality

A 6000x4000 test picture with a zone plate, ruled lines and small text was
captured fitted on both builds and compared with a Lanczos reduction of the
original. The copy, reduced once with Qt's smooth scaling, was closer to it
(24.2 dB PSNR against 22.5 dB) and showed visibly less moiré in the zone plate;
text was equally sharp. Zooming past the copy shows the original's pixels.

## Limits

Three runs per build on generated fixtures; no physical graphics card, Wayland
presentation or video memory was measured. A picture up to a quarter longer
than the screen is still shown whole, so the viewer only enlarges past copies at
least a tenth smaller than the picture.
