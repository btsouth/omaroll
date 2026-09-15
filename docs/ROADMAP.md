# Roadmap

Omaroll should cover everyday media viewing and organization on Omarchy:
open files quickly, find them again, make a quick correction, and share the
result. Discovery stays read-only and core use stays offline.

See [current status and handoff](STATUS.md) for release boundaries, evidence
and the list of things we decided not to build.

## 1.8.0 (merged to main, unreleased)

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

Nothing else is scheduled for 1.8.0. Cutting the release is the remaining work:
bump the version, AppStream entry, changelog and README, run the Release and
sanitizer validation, test the candidate on the real desktop, then tag and
verify the published artifacts.

## Open questions

- **PDF links and on-page text selection.** Both need a Poppler link/text API
  that the pdftoppm-based integration does not expose. The choice is either
  Poppler's link API (not available through the CLI we use) or adopting the
  QtPdf module, which adds `qt6-pdf` to the CI, release and PKGBUILD
  dependency lists. Decide this before building either.
- **Physical desktop gates.** Installed field acceptance and Wayland/GPU
  presentation numbers and regression budgets need the real Omarchy session.

## Not doing (decided, with reasons)

Do not add these without a fresh decision; they are bloat or duplication:

- **XMP sidecar read/write.** Niche here; writing sidecars breaks the
  read-only organization ethos, and the versioned JSON backup covers
  portability.
- **Video chapters, play queue and loop.** Serious playback is delegated to
  `mpv`; rebuilding a player duplicates it.
- **Subtitle styling and a separate named-track picker.** Tracks are already
  named in the CC cycle and the default rendering is readable.
- **Offline Places from EXIF GPS.** A bundled geodata table is package weight
  for a niche view.
- **PDF selectable-text panel.** Duplicated the existing Copy page text action.
- **A duplicate/similar review flow.** Already covered by the duplicate and
  similar filters, the Keep selected action and the compare view.
- **Target-monitor fullscreen.** Hyprland owns window placement; forcing a
  screen is fragile.

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
- Maintain the [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295),
  which currently points at v1.7.0.

Repository inclusion, installation by default and MIME defaults are separate
upstream decisions. Advanced editing remains available through Omarchy's
existing tools.

Report concrete missing workflows and reproducible problems in
[GitHub issues](https://github.com/btsouth/omaroll/issues).
