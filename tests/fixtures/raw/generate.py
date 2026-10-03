#!/usr/bin/env python3
"""Regenerate the camera raw test files. Needs only the standard library.

Two tiny DNGs that LibRaw reads the way it reads a camera's:

- camera.dng: a 64x48 RGGB mosaic, a 32x24 RGB preview, an orientation that
  turns the picture 90 degrees clockwise, and the EXIF fields Omaroll shows.
- no-preview.dng: the same without the preview, which LibRaw has to decode.

The top half of each is bright and the bottom half dark, so the picture the
right way up is bright on the right.
"""
from pathlib import Path
import struct

BYTE, ASCII, SHORT, LONG, RATIONAL, SRATIONAL = 1, 2, 3, 4, 5, 10
SIZES = {BYTE: 1, ASCII: 1, SHORT: 2, LONG: 4, RATIONAL: 8, SRATIONAL: 8}
FORMATS = {BYTE: '<B', SHORT: '<H', LONG: '<I', RATIONAL: '<II', SRATIONAL: '<ii'}

RAW_WIDTH, RAW_HEIGHT = 64, 48
PREVIEW_WIDTH, PREVIEW_HEIGHT = 32, 24


def encode(kind, values):
    if kind == ASCII:
        return values.encode() + b'\0'
    return b''.join(struct.pack(FORMATS[kind], *(v if isinstance(v, tuple) else (v,)))
                    for v in values)


def tiff(directories, blobs):
    """Little-endian TIFF. A value naming a directory or blob is its offset."""
    layout = {}
    at = 8
    for name, entries in directories:
        layout[name] = at
        at += 2 + 12 * len(entries) + 4
    overflow = at
    for _, entries in directories:
        for kind, values in ((kind, values) for _, kind, values in entries):
            size = len(values) + 1 if kind == ASCII else SIZES[kind] * len(values)
            if size > 4:
                overflow += size + size % 2
    for name, data in blobs:
        layout[name] = overflow
        overflow += len(data)

    out = bytearray(b'II*\0' + struct.pack('<I', 8))
    extra = bytearray()
    extra_at = at
    for _, entries in directories:
        out += struct.pack('<H', len(entries))
        for tag, kind, values in sorted(entries):
            values = [layout.get(v, v) if isinstance(v, str) else v for v in values] \
                if kind != ASCII else values
            data = encode(kind, values)
            count = len(data) if kind == ASCII else len(values)
            if len(data) <= 4:
                out += struct.pack('<HHI', tag, kind, count) + data.ljust(4, b'\0')
            else:
                out += struct.pack('<HHII', tag, kind, count, extra_at + len(extra))
                extra += data + b'\0' * (len(data) % 2)
        out += struct.pack('<I', 0)
    out += extra
    for _, data in blobs:
        out += data
    return bytes(out)


def mosaic():
    return b''.join(struct.pack('<H', 52000 if y < RAW_HEIGHT // 2 else 6000)
                    for y in range(RAW_HEIGHT) for _ in range(RAW_WIDTH))


def preview():
    return b''.join(bytes((235, 235, 235) if y < PREVIEW_HEIGHT // 2 else (40, 40, 40))
                    for y in range(PREVIEW_HEIGHT) for _ in range(PREVIEW_WIDTH))


def camera_tags():
    return [
        (0x010F, ASCII, 'Omaroll'),
        (0x0110, ASCII, 'Test Camera'),
        (0x0112, SHORT, [6]),
        (0x8769, LONG, ['exif']),
        (0xC612, BYTE, [1, 4, 0, 0]),
        (0xC613, BYTE, [1, 1, 0, 0]),
        (0xC614, ASCII, 'Omaroll Test Camera'),
        (0xC621, SRATIONAL, [(1, 1), (0, 1), (0, 1), (0, 1), (1, 1), (0, 1),
                             (0, 1), (0, 1), (1, 1)]),
        (0xC628, RATIONAL, [(1, 1), (1, 1), (1, 1)]),
        (0xC65A, SHORT, [21]),
    ]


def mosaic_tags(subfile):
    return [
        (0x00FE, LONG, [subfile]),
        (0x0100, LONG, [RAW_WIDTH]),
        (0x0101, LONG, [RAW_HEIGHT]),
        (0x0102, SHORT, [16]),
        (0x0103, SHORT, [1]),
        (0x0106, SHORT, [32803]),
        (0x0111, LONG, ['mosaic']),
        (0x0115, SHORT, [1]),
        (0x0116, LONG, [RAW_HEIGHT]),
        (0x0117, LONG, [RAW_WIDTH * RAW_HEIGHT * 2]),
        (0x011C, SHORT, [1]),
        (0x828D, SHORT, [2, 2]),
        (0x828E, BYTE, [0, 1, 1, 2]),
        (0xC61D, LONG, [65535]),
    ]


EXIF = ('exif', [
    (0x829A, RATIONAL, [(1, 125)]),
    (0x829D, RATIONAL, [(28, 10)]),
    (0x8827, SHORT, [400]),
    (0x9003, ASCII, '2026:09:30 14:05:06'),
    (0x920A, RATIONAL, [(35, 1)]),
    (0xA434, ASCII, 'Test Lens 35mm F2.8'),
])

root = Path(__file__).resolve().parent
with_preview = [
    ('preview', camera_tags() + [
        (0x00FE, LONG, [1]),
        (0x0100, LONG, [PREVIEW_WIDTH]),
        (0x0101, LONG, [PREVIEW_HEIGHT]),
        (0x0102, SHORT, [8, 8, 8]),
        (0x0103, SHORT, [1]),
        (0x0106, SHORT, [2]),
        (0x0111, LONG, ['pixels']),
        (0x0115, SHORT, [3]),
        (0x0116, LONG, [PREVIEW_HEIGHT]),
        (0x0117, LONG, [PREVIEW_WIDTH * PREVIEW_HEIGHT * 3]),
        (0x011C, SHORT, [1]),
        (0x014A, LONG, ['raw']),
    ]),
    ('raw', mosaic_tags(0)),
    EXIF,
]
(root / 'camera.dng').write_bytes(
    tiff(with_preview, [('pixels', preview()), ('mosaic', mosaic())]))
(root / 'no-preview.dng').write_bytes(
    tiff([('raw', camera_tags() + mosaic_tags(0)), EXIF], [('mosaic', mosaic())]))
