# Roadmap

Omaroll should cover everyday media viewing and organization on Omarchy:
open files quickly, find them again, make a quick correction, and share the
result. Discovery stays read-only and core use stays offline.

See [current status and handoff](STATUS.md) for release boundaries, evidence
and the scope decisions.

## Unreleased

- Portrait crop presets (3:4, 2:3, 9:16), and the correction controls grouped
  into Crop, Rotate and Resize rows that wrap only within themselves, with the
  output and the actions beside them when the sheet is wide.
- Select text in a PDF: drag across a page, see the selected lines highlighted,
  and copy just those words. The boxes come from `pdftotext -bbox`, a mode of
  the same Poppler tool the document search and page-text copy already run, so
  no new package is involved.
- Fix the two PDF surfaces reading the renderer's requested size as if it were
  the page's own size, which collapsed every page in the continuous view once
  it rendered and stretched a fitted page to the stage box's aspect.
- Secondary text takes the theme's muted colour, which the theme backend
  already picks for at least 3:1 contrast, instead of fixed alphas that measured
  about 1.7:1 on a light theme: the status line, section labels, placeholders,
  empty rating stars and action shortcuts.
- PDF pages draw a hairline edge, so white paper reads as a sheet on a light
  theme rather than merging with the stage behind it.
- The document control rows give way in order on a narrow window, measured in
  the theme font: the match steppers first, then the pills the action list
  repeats or that need more room, with the page, its navigation, the fit choice
  and text selection kept to the smallest window the app allows.
- The render matrix covers every view in the dark palette, the chrome-heavy
  views in the light palette, and the smallest window, each against a theme
  fixture under `tests/fixtures/themes/` so the palette does not depend on the
  machine. The demo library gains a document, so the PDF surfaces are rendered
  and reviewed like every other view.

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
  in-process page rendering, but its bindings are GPL, so linking them would
  relicense the application. Decide this before building it.
- **Physical desktop gates.** Installed field acceptance and Wayland/GPU
  presentation numbers and regression budgets need the real Omarchy session.
  Headless runs cannot close them.

## Out of scope

Considered for 1.8.0 and left out; revisit only with a fresh decision:

- **XMP sidecars.** The versioned JSON backup already covers portability, and
  writing sidecars would go against the read-only organization policy.
- **Video chapters, play queue and loop.** Playback is delegated to `mpv`.
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
- Maintain the [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295),
  which currently points at v1.8.0.

Repository inclusion, installation by default and MIME defaults are separate
upstream decisions. Advanced editing remains available through Omarchy's
existing tools.

Report concrete missing workflows and reproducible problems in
[GitHub issues](https://github.com/btsouth/omaroll/issues).
