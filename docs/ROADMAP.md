# Roadmap

Omaroll should cover everyday media viewing and organization on Omarchy:
open files quickly, find them again, make a quick correction, and share the
result. Discovery stays read-only and core use stays offline.

See [project status](STATUS.md) for the latest release and current work.

## Current priorities

- Refine the quick viewer and library together: navigation, search, selection,
  clear feedback, and controls that work in narrow windows and both themes.
- Use the [continuous-input comparison](performance/2026-10-03-gallery-loading/README.md)
  when evaluating gallery scheduling. Preserve cached navigation and distinguish
  submitted frames from desktop presentation and physical audio.
- Improve desktop integration and reliability from reproducible problems and
  user feedback. Preserve originals and keep specialist editing in the
  existing tools.
- Deep zoom for very large JPEGs: decode only the region on screen, so full
  resolution needs no more memory than the screen and pictures longer than a
  texture are sharp too. Very large pictures currently open from a reduced copy
  and load full resolution whole when it fits
  ([#115](https://github.com/btsouth/omaroll/issues/115)).

## 1.10.0 and 1.11.0 (released)

- Multiple viewers, with repeated opens bringing the existing viewer forward.
  On Hyprland, viewers opened together tile instead of covering each other.
- Only the last-used video viewer is heard; the others keep playing silently.
- Images and videos without extensions use content detection for opening,
  navigation, library previews and thumbnails.
- Portrait crop presets (3:4, 2:3, 9:16), grouped correction controls, and
  subtitle timing buttons that leave room for the video clock and seek bar.

See the [changelog](../CHANGELOG.md) for release details and contributor credits.

## 1.9.1 (released)

- Compact library preview headers, an optional inspector and an overflow menu.
- Responsive video controls and Resume/Restart choices.
- Caption edits saved before navigation, and keyboard focus restored after
  nested actions.

## 1.9.0 (released)

- A quick image and video viewer that opens files without building the library,
  browses neighboring files, zooms and pans, plays video, and opens the library
  on Enter.
- PDF text selection: drag across a page and copy just the selected words.
- Correct PDF page sizing, readable secondary text in both themes, and document
  controls that fit narrow windows.
- Hidden unavailable audio and subtitle controls, plus wheel and touchpad
  handling that keeps panning separate from zoom.
- A demo PDF and a render matrix covering every view in the dark palette,
  the main controls in the light palette, and narrow windows.

See the [release](https://github.com/btsouth/omaroll/releases/tag/v1.9.0)
and [changelog](../CHANGELOG.md#190).

## 1.8.0 (released)

Ten focused PRs, each squash merged with green CI, the isolated core and UI
suites and the OpenGL runner:

- Lossless JPEG rotate and flip (jpegtran, with fallbacks for crops, resizes,
  non-JPEG and EXIF-oriented sources).
- Crop aspect presets (Free/Original/1:1/4:3/3:2/16:9) held while the frame is
  dragged, and a ±15° straighten that fills the frame.
- Correct a selection (B): rotate, flip or resize a whole selection as copies.
- Verification that a transparent PNG keeps its alpha and a 400-megapixel
  resize stays inside the output budget.
- PDF continuous scroll with Fit width / Fit page modes.
- Print pictures and PDFs through CUPS `lp`.
- Ctrl+Z undo for favourites, hidden flags, ratings and captions.
- Sidecar subtitle timing nudge (±0.5 s).
- Slideshow interval and shuffle options.

[Released](https://github.com/btsouth/omaroll/releases/tag/v1.8.0) from PR #45
(8f216b8); the published artifacts were verified against SHA256SUMS. Physical
desktop acceptance was not recorded for this tag and remains in SBS-1121.

## Open questions

- **PDF links.** On-page text selection is delivered, since `pdftotext -bbox`
  already reports a box per word. Links are what remains, and the two options
  are now measured rather than assumed. QtPdf provides link, bookmark and
  search models, but on Arch it ships inside `qt6-webengine` (282 MiB
  installed on the development desktop) and its QML module is not installed
  there, so it would be used through its C++ API. Linking `poppler-qt6`
  instead costs about 270 KiB and gives links, word boxes, an outline and
  in-process page rendering, but its bindings are GPL. Review the dependency
  footprint and license compatibility with Omaroll before choosing an integration.
- **Physical desktop gates.** Installed field acceptance and Wayland/GPU
  presentation numbers and regression budgets need the real Omarchy session.
  Headless runs cannot close them.

## Out of scope

Considered for 1.8.0 and left out; revisit only with a fresh decision:

- **XMP sidecars.** The versioned JSON backup already covers portability, and
  writing sidecars would go against the read-only organization policy.
- **Video chapters, play queue and loop.** The viewer plays video; these
  additional controls are outside the current scope.
- **Subtitle styling and a separate track picker.** Tracks are already named
  and the default rendering is readable.
- **Offline Places from EXIF GPS.** Needs a bundled geodata table.
- **PDF selectable-text panel.** Copy page text already covers it.
- **A duplicate/similar review flow.** Provided by the filters, Keep selected
  and compare.
- **Target-monitor fullscreen.** Hyprland owns window placement.

## Shipped before 1.8.0

- Crop, rotate, flip and resize with Save a copy, Copy region and safe
  collision handling; orientation baked in and ICC preserved.
- Compare with synchronized zoom and pan, from a selection or a
  duplicate/similar set.
- Albums, nested tags, captions, ratings, favourites, hidden files and saved
  views, with rename that keeps membership and a versioned backup/restore.
- Favourites, ratings and captions follow an external move or rename.
- Video volume, mute and resume; sidecar `.srt`/`.vtt` subtitles.
- PDF page navigation, page-number jump, text search with match stepping, and
  Copy page text.

## Reliability, scale and adoption

- Maintain the animated-image, video-track, subtitle and rendered-pixel checks
  already running in CI. Local tests stay audio-isolated.
- Extend the benchmarks with warm navigation and 50k mixed libraries where
  they can run headless; the Wayland presentation and GPU memory numbers need
  the desktop.
- The [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295)
  is under review.

Repository inclusion, installation by default and MIME defaults are separate
upstream decisions. Advanced editing remains available through Omarchy's
existing tools.

Report concrete missing workflows and reproducible problems in
[GitHub issues](https://github.com/btsouth/omaroll/issues).
