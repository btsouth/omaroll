# Roadmap

Omaroll should cover everyday media viewing and organization on Omarchy:
open files quickly, find them again, make a quick correction, and share the
result. Discovery stays read-only and core use stays offline.

See [current status and handoff](STATUS.md) for release boundaries and evidence.

## 1.8.0 plan

Each item ships as its own focused PR with tests, in this order. Linear tracks
them under the `v1.8.0` milestone.

### Image quality for photographers

- **Lossless JPEG rotate and flip.** Done: a JPEG rotated or flipped with no
  crop or resize is written without recompressing the pixels.
- **Straighten and crop aspect presets.** Done: Free/Original/1:1/4:3/3:2/16:9
  crop presets hold the crop to the chosen ratio, and a ±15° straighten control
  fills the frame so no empty corners show.
- **Batch apply corrections.** Done: rotate, flip or resize a whole selection
  at once (B), each as its own copy with the existing collision handling.
- Finish the remaining verification: transparency and very large images. Done:
  tests cover a transparent PNG keeping its alpha through a correction copy and
  a 400-megapixel resize staying inside the output budget.

### PDF reading

- **Continuous scroll and fit modes.** Done: pages scroll at the window width
  by default, with a Fit page mode for whole-page reading and zoom/pan.
- **Links** extracted per page and opened from the viewer.
- **Range text selection** over a per-word text layer, with Copy page text as
  the fallback.
- **Printing** for pictures and PDFs through the system print path. Done: a
  Print action hands the file to CUPS and is greyed out when lp is absent.

### Organization

- **XMP sidecar read** (and optional write) so tags, ratings and captions are
  portable; pairs with backup and restore.
- **Undo** for album/tag removal, hide, rating and caption changes. Partial:
  Ctrl+Z undoes favourites, hidden flags, ratings and captions; album and tag
  membership changes are not undoable yet.
- **Duplicate and similar review flow:** keep or reject each item and jump to
  the next set. Similarity never deletes automatically.

### Video

- **Subtitle offset and styling**, and a named track picker instead of cycling.
  Offset is done for sidecar subtitles (±0.5 s steps). Styling and a separate
  picker were dropped: the tracks are already named in the CC cycle and the
  default rendering is readable, so they would be surface for its own sake.
- **Chapters, a play queue and loop**, remembering position across the queue.

### Desktop integration

- **Slideshow options:** interval, shuffle and a simple transition.
- **Target-monitor fullscreen.**
- **Offline Places** from EXIF GPS with a small bundled geodata table; no map.

### Reliability, scale and adoption

- Extend the benchmarks with warm navigation and 50k mixed libraries, record
  Wayland presentation and GPU memory on the desktop, and set regression
  budgets.
- Maintain the animated-image, video-track, subtitle and rendered-pixel checks
  already running in CI. Local tests stay audio-isolated.
- Finish installed Omarchy acceptance: file-manager opening, scaling,
  clipboard, drag/drop, window state and physical audio (SBS-1121).
- Maintain the [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295).

## Shipped

- Crop, rotate, flip and resize with Save a copy, Copy region and safe
  collision handling; orientation baked in and ICC preserved.
- Compare with synchronized zoom and pan, from the selection or a
  duplicate/similar set.
- Albums, nested tags, captions, ratings, favourites, hidden files and saved
  views, with rename that keeps membership and a versioned backup/restore.
- Favourites, ratings and captions follow an external move or rename.
- Video volume, mute and resume; sidecar `.srt`/`.vtt` subtitles.
- PDF page navigation, page-number jump, text search with match stepping, and
  Copy page text.

Each phase ships in useful increments. Repository inclusion, installation by
default and MIME defaults are separate upstream decisions. Image defaults are
the first adoption target; video and PDF need their own acceptance checks.
Advanced editing remains available through Omarchy's existing tools.

Report concrete missing workflows and reproducible problems in
[GitHub issues](https://github.com/btsouth/omaroll/issues).
