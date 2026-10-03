# Camera raw fixtures

Original, generated DNG files covered by the repository's MIT license. They are
a few kilobytes each, so the raw tests run in CI without camera files or
downloads.

- `camera.dng`: a 64x48 RGGB mosaic with a 32x24 RGB preview, an orientation
  that turns it 90 degrees clockwise, and a camera, lens, date and exposure in
  its EXIF.
- `no-preview.dng`: the same mosaic and tags without a preview.

The top half of each is bright and the bottom half dark, so a picture shown
the right way up is bright on the right.

Regenerate with `python tests/fixtures/raw/generate.py`, which needs only the
standard library. Decoding them needs LibRaw, which loads the kimageformats raw
plugin; the tests skip without it.
