# Viewer fixtures

These are original, generated test files covered by the repository's MIT license.
No third-party downloads or attribution are required. The checked-in files allow
CI to run without Pillow or network access.

- `animated.gif` and `animated.webp`: 12 transparent frames, 120 ms per frame,
  infinite loop, a moving orange block, and numbered frames. GIF uses background
  disposal to expose stale-frame artifacts.
- `still.webp` and `transparent.png`: the first frame, without animation.
- `truecolor.tga`: uncompressed 32-bit Truevision 2.0, including its footer.
- `tracks.mkv`: a 12-second moving test pattern, two mono AAC tracks (440 Hz and
  880 Hz), and two timed English subtitles. Audio language labels distinguish
  the tracks; the content is a tone.
- `captions.srt`: the subtitle source.

Regenerate with `python tests/fixtures/viewer/generate.py` using Pillow and FFmpeg.
Open the media files directly in Omaroll for manual checks. Automated UI tests
copy fixtures into disposable libraries and use the offscreen Qt platform.
