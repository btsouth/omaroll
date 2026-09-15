# Project status and handoff

Snapshot: 15 September 2026 UTC. v1.7.0 is the latest release; 1.8.0 work is
merged to `main` but unreleased. Check the latest PRs and the release before
resuming.

## Released

[v1.7.0](https://github.com/btsouth/omaroll/releases/tag/v1.7.0) is the latest
public release, tagged from
[PR #30](https://github.com/btsouth/omaroll/pull/30) (0c9fb44). The Release
workflow built the Arch package, tested install, upgrade, reinstall and
removal, and published the source archive, package, PKGBUILD and checksums
with signed provenance; all four were downloaded and verified against
SHA256SUMS. It bundles image corrections and region copy, side-by-side compare,
album and tag rename, organization backup and restore, favourites, ratings and
captions that follow external moves, remembered video volume and resume,
sidecar subtitles, and PDF search, page jump and page-text copy. The release
candidate passed the Release build, the isolated core and UI suites, the
OpenGL media runner, metadata validation, a staged install, a full ASan/UBSan
run with every deterministic render inspected. Physical desktop acceptance
(installed behaviour, clipboard, drag/drop, scaling and audio) was not recorded
for this tag; it remains tracked in SBS-1121.

[v1.6.0](https://github.com/btsouth/omaroll/releases/tag/v1.6.0) preceded it,
tagged from [PR #19](https://github.com/btsouth/omaroll/pull/19) (3f4d437). It
adds star ratings, captions, nested tags, camera and lens browsing, Tab cycling
of the sections and a held first frame when a video ends.

[v1.5.0](https://github.com/btsouth/omaroll/releases/tag/v1.5.0) preceded it
the same day, tagged from
[PR #7](https://github.com/btsouth/omaroll/pull/7) (64ddae8). The viewer supports
still and animated images, embedded video controls, PDF paging, OCR/QR, albums,
tags, saved collections and duplicate review. The README describes the
supported formats and workflows.

## 1.8.0 in progress (merged to main, unreleased)

Ten focused PRs landed after 1.7.0, each squash merged with green CI, the
isolated core and UI suites and the OpenGL runner:

- [PR #33](https://github.com/btsouth/omaroll/pull/33): lossless JPEG rotate and
  flip through jpegtran, with fallbacks for crops, resizes, non-JPEG and
  EXIF-oriented sources.
- [PR #34](https://github.com/btsouth/omaroll/pull/34): crop aspect presets
  (Free/Original/1:1/4:3/3:2/16:9) held while the frame is dragged.
- [PR #35](https://github.com/btsouth/omaroll/pull/35): a ±15° straighten that
  fills the frame so no empty corners show.
- [PR #36](https://github.com/btsouth/omaroll/pull/36): Correct a selection (B),
  rotate/flip/resize a whole selection as copies.
- [PR #37](https://github.com/btsouth/omaroll/pull/37): PDF continuous scroll
  with Fit width / Fit page modes.
- [PR #38](https://github.com/btsouth/omaroll/pull/38): Print pictures and PDFs
  through CUPS `lp`.
- [PR #39](https://github.com/btsouth/omaroll/pull/39): verification for
  transparency and very large images in corrections.
- [PR #40](https://github.com/btsouth/omaroll/pull/40): Ctrl+Z undo for
  favourites, hidden flags, ratings and captions.
- [PR #41](https://github.com/btsouth/omaroll/pull/41): sidecar subtitle timing
  nudge (±0.5 s).
- [PR #42](https://github.com/btsouth/omaroll/pull/42): slideshow interval and
  shuffle options.

The changelog carries all of it under `## Unreleased`.

## Decisions: what we are not building

These came up in the 1.8.0 plan and were deliberately dropped to keep the app
full-featured without bloat. Do not add them without a fresh decision:

- **XMP sidecar read/write.** Niche for this app, writing sidecars breaks the
  read-only organization ethos, and the versioned JSON backup already covers
  portability.
- **Video chapters, play queue and loop.** Serious playback is delegated to
  `mpv` through the Play action; rebuilding a player duplicates it.
- **Subtitle styling and a separate named-track picker.** Tracks are already
  named in the CC cycle and the default rendering is readable, so these are
  surface without new capability.
- **Offline Places from EXIF GPS.** A bundled geodata table is package weight
  for a niche view in a general viewer.
- **PDF selectable-text panel.** It duplicated the existing Copy page text
  action; a second text affordance is not worth it. (A proper on-page range
  selection is a separate question, below.)
- **A duplicate/similar review flow.** Already covered: duplicate and similar
  filters, a Keep selected action that trashes the other copies with a
  confirmation, and the compare view.
- **Target-monitor fullscreen.** Hyprland owns window placement, so forcing a
  screen is fragile; leave it to the compositor.

## Open questions

- **PDF links and on-page text selection.** Both need a Poppler link/text API
  that the pdftoppm-based integration does not expose. The choice is either
  Poppler's link API (not available through the CLI we use) or adopting the
  QtPdf module, which adds `qt6-pdf` to the CI, release and PKGBUILD
  dependency lists. This is the integration decision SBS-1134 recorded; make
  it before building either.
- **Physical desktop gates.** SBS-1121 field acceptance and SBS-1122
  Wayland/GPU presentation numbers and regression budgets need the real
  Omarchy session. Headless runs cannot close them.

## What 1.5.0 bundled

Main carries the 1.5.0 version, changelog and AppStream entry. It bundles:

- [PR #3](https://github.com/btsouth/omaroll/pull/3): home-directory wrapper cleanup.
- [PR #4](https://github.com/btsouth/omaroll/pull/4): natural filename sorting,
  accessible OCR copy feedback and rendered media checks in CI/release validation.
- [PR #5](https://github.com/btsouth/omaroll/pull/5): ordered multi-file opening,
  selection forwarding to an existing window, performance baselines and isolated
  media tests.
- [PR #6](https://github.com/btsouth/omaroll/pull/6): deferred video player and
  display until a video is opened, plus startup readiness tracing and a
  benchmark probe. The first video still pays the initialization cost.
- [PR #8](https://github.com/btsouth/omaroll/pull/8): async image responses
  emit `finished()` on their own thread, fixing a use-after-free that the
  pixmap reader's `deleteLater()` could trigger while the pool thread was
  still unwinding the emit.
- [PR #9](https://github.com/btsouth/omaroll/pull/9): grid delegates are
  rebuilt rather than reused across the tile relayout, fixing tiles that kept
  a previous file's thumbnail and opened a different file in small tiled
  windows. Found in the desktop test of the 1.5.0 candidate, which also
  covered image, video with live audio, PDF and multi-file forwarding.

CodeRabbit reviewed PR #6 with one minor finding, which was fixed before the
squash merge. Core, UI, sanitizer and OpenGL checks passed locally and in CI.
The intermittent SIGSEGV in `omaroll_ui_tests` seen once on 2026-09-04 was
traced through its core dump to the response lifetime race fixed in PR #8.

[Local measurements](performance/2026-09-05-startup/README.md) record medians
of 949.9 to 257.2 ms for an image-ready submitted frame and 1181.2 to 471.9 ms
for a ready grid. These are offscreen NVIDIA measurements from main entry,
not compositor presentation or general performance guarantees.

## What 1.6.0 bundled

Four feature PRs landed after the release, each squash merged with green CI and
passing core and UI suites in the sandbox. All were tested on the real Omarchy
desktop on 5 September 2026, which found three problems fixed in
[PR #16](https://github.com/btsouth/omaroll/pull/16) (Shift+Tab was an
ambiguous shortcut), [PR #17](https://github.com/btsouth/omaroll/pull/17)
(caption save depended on focus; a finished video left a blank stage) and
[PR #18](https://github.com/btsouth/omaroll/pull/18) (header count wording).
Version 1.6.0 carries all of it, plus the 1.6.0 preparation in PR #19:

- [PR #11](https://github.com/btsouth/omaroll/pull/11): Tab and Shift+Tab cycle
  the section pills.
- [PR #12](https://github.com/btsouth/omaroll/pull/12): the metadata pass reads
  camera and lens as well as dates, Browse gains a Cameras section, and
  MediaDateIndex became MediaMetadataIndex with a new cache file.
- [PR #13](https://github.com/btsouth/omaroll/pull/13): star ratings with Alt+1
  to Alt+5, a Top rated sort and a minimum rating filter in Browse.
- [PR #14](https://github.com/btsouth/omaroll/pull/14): tags nest with a slash
  and files take a caption in the viewer that is searched with names and text.

The changelog lists them under 1.6.0. A 31 second demo recording of the
release lives at ~/Videos/omaroll-1.6.0-demo.mp4 on the development desktop. Semantic search and face
recognition were considered against Lightroom and Immich and deferred
(SBS-1139); do not add ML dependencies without a fresh decision.

## What 1.7.0 bundles

1.7.0 carries everything below, all merged to `main` in PRs #21 through #29
with core and UI tests green through `tests/run-isolated.sh` and the OpenGL
runner:

- **SBS-1126 safe image corrections.** `src/edit/ImageEditor` and
  `EditProvider` implement orient, crop, quarter turns, flips and resize, then
  Save a copy with numbered collision handling and ICC preservation. The
  viewer's new Crop, rotate, resize (Q) action opens `CorrectionSheet.qml`,
  a preview from `image://edit/` with a draggable crop frame. The original is
  never written.
- **SBS-1130 collection rename.** `AppSettings::renameAlbum` and `renameTag`
  move membership with the name; a nested tag rename carries its descendants.
  Browse gains Rename selected.
- **SBS-1129 organization backup.** `AppSettings::exportOrganization` and
  `importOrganization` write and read a versioned JSON snapshot (albums, tags,
  favourites, hidden, ratings, captions, saved views) atomically, validating
  the whole file before any change. Settings gains Back up and Restore.
- **SBS-1127 comparison.** `CompareSheet.qml` shows the checked selection, or
  the open picture's exact-duplicate then visually-similar set, side by side
  with one shared zoom and pan, driven by a new `SimilarityIndex::groupPaths`.
- **SBS-1131 marks recovery.** Favourites, hidden flags, ratings and captions
  now store the file's identity when marked and follow an external move or
  rename through `AppSettings::reconcileMarks`, which runs before the dead-path
  sweep. Albums and tags already had this; marks did not.
- **SBS-1132 playback memory.** Volume and mute persist, and a video reopened
  part-way offers Resume or Start over from a saved spot (pruned to 500
  entries). Playback that reaches the end clears the spot.
- **SBS-1133 external subtitles.** `src/subtitles/SubtitleIndex` finds a `.srt`
  or `.vtt` beside a video, parses it into cached cues and answers by playback
  position; the CC control cycles Off, embedded tracks and sidecars, and the
  cue is drawn over the video. The transport controls were re-anchored, since
  the positioner did not lay out the late-appearing CC button.
- **SBS-1134 PDF work.** `PdfInspector::find` runs pdftotext and
  `PdfSupport::findPages` matches across pages, case- and whitespace-insensitive;
  the viewer gains a find box, a match count and previous/next match that turn
  the page, a page-number jump, and Copy page text through `ClipboardText`.
  Range selection, links and printing remain.
- **SBS-1127 region copy.** The crop editor's Copy region puts the selected
  area, after rotation and flip, on the clipboard through `ClipboardImage`
  without writing a file. Synchronized-zoom comparison is delivered separately.
- **SBS-1128 orientation and color checks.** Tests cover an EXIF-oriented JPEG,
  read upright and baked into a correction copy, and an ICC profile surviving a
  JPEG correction. Transparency and very large images are still to verify.

Remaining after this branch: region copy (SBS-1127 remainder), orientation and
color verification fixtures (SBS-1128), and the rest of PDF depth (selection,
links, printing).

## Next decisions and release gates

1. Keep new changes in separate focused PRs off main, following
   [RELEASING.md](../RELEASING.md) for the next tag.
2. Decide the PDF integration (Poppler link API versus the QtPdf dependency)
   before building PDF links or on-page text selection. See Open questions.
3. Finish installed Omarchy acceptance (SBS-1121): file-manager selections,
   clipboard, drag/drop, scaling, window state and physical audio. Headless
   passes do not close this gate.
4. Continue performance work (SBS-1122): warm navigation, larger and mixed
   libraries, Wayland presentation, GPU memory and realistic regression budgets.
5. Prepare and cut 1.8.0: bump the version, AppStream entry, changelog and
   README, run the Release and sanitizer validation, test the candidate on the
   real desktop, then tag. Nothing is scheduled for 1.8.0 beyond the ten merged
   PRs and the two open questions.
6. Watch the Omarchy package submission for upstream feedback.

[Official package PR #295](https://github.com/omacom/omarchy-pkgs/pull/295)
remains open and points at v1.7.0; upstream acceptance is pending. Earlier
edge, rc and stable package builds passed. Repository inclusion, default
installation and MIME defaults are separate upstream decisions.

## Safe continuation

Use [the validation commands](../tests/README.md), always through
`tests/run-isolated.sh` on this desktop. Earlier tests played 440/880 Hz fixture
tones because blocking PulseAudio alone did not block Qt's native PipeWire
backend. PR #5 isolates both backends, hides physical audio devices and session
sockets, and stubs test notifications and clipboard helpers. Preserve this
isolation; do not change host audio settings to make a test pass.

The checked-in fixtures cover GIF, animated/still WebP, transparency, TGA,
video tracks and subtitles. No media downloads are needed for those checks.
Physical audio testing remains a deliberate desktop acceptance activity.

Linear is the active task tracker. Access it through Toolport. SBS-1093 is the
parent roadmap; SBS-1121 (field acceptance) and SBS-1122 (performance) remain
in progress and need the desktop, SBS-1134 (PDF depth) stays open pending the
integration decision, and the v1.8.0 milestone carries the shipped items. Notes
under `build/roadmap-review/` are historical and ignored by Git. This file is
the committed handoff.
