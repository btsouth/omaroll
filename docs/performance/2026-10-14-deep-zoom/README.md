# Deep zoom for very large JPEGs, 14 October 2026

A JPEG whose full decode would pass 256 MiB opened from a copy the size of the
screen, and zooming past that copy decoded the whole picture and uploaded it as
one texture. A 150-megapixel scan needed about 600 MiB for the decode and as
much again on the graphics card, and a picture longer than 16384 pixels could
not be shown at full resolution at all.

The quick viewer now decodes only the part on screen once the view settles,
with a quarter of the view as margin on each side, at the screen's resolution
and never past the picture's own pixels. The copy shows underneath while an
area loads, and the last area stays until the next one arrives. Smaller photos
and formats that cannot decode part of themselves keep loading full resolution
whole. The library's detail sheet is unchanged.

## Decode time

Intel Core i7-14700F, Qt 6.12.0. A generated 15000x10000 JPEG at quality 90,
12 MiB on disk. One read each through `QImageReader`:

| Read | Time |
| --- | ---: |
| Whole picture | 912 ms |
| 2560x1440 area, top left | 82 ms |
| 2560x1440 area, middle | 176 ms |
| 2560x1440 area, bottom right | 290 ms |
| 5120x2880 area reduced to 2560x1440 | 124 ms |

libjpeg still reads every row above an area, so areas lower in the picture
take longer. A real scan with more detail compresses less and reads slower.

## Memory

The same picture in an isolated Omarchy desktop at 2560x1440, rendering on the
NVIDIA graphics card, process RSS after settling:

| | main (`81a79fc`) | This change |
| --- | ---: | ---: |
| Fitted | 350 MiB | 346 MiB |
| Actual size | 1.47 GiB | 410 MiB |
| Actual size, after panning | | 411 MiB |

Text in the picture was sharp at actual size, after panning and with the view
turned a quarter.

## Limits

One generated picture on one machine. RSS includes the graphics driver's
mappings, and video memory was not measured on its own. While a pan or zoom is
in progress the copy is drawn, so fine detail sharpens a moment after the view
stops.
