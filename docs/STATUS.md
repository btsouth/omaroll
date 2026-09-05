# Project status and handoff

Snapshot: 5 September 2026 UTC, with 1.6.0 prepared after the v1.5.0 release and
the feature merges that followed. Check the linked PRs and release before resuming.

## Released

[v1.5.0](https://github.com/btsouth/omaroll/releases/tag/v1.5.0) is the latest
public release, tagged from
[PR #7](https://github.com/btsouth/omaroll/pull/7) (64ddae8) after a desktop
test on the real Omarchy session. The Release workflow published the source
archive, Arch package, PKGBUILD and checksums; all four were downloaded and
verified. The viewer supports still and animated images, embedded video
controls, PDF paging, OCR/QR, albums, tags, saved collections and duplicate
review. The README describes the supported formats and workflows.

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

## 1.6.0 candidate

Four feature PRs landed after the release, each squash merged with green CI and
passing core and UI suites in the sandbox. All were tested on the real Omarchy
desktop on 5 September 2026, which found three problems fixed in
[PR #16](https://github.com/btsouth/omaroll/pull/16) (Shift+Tab was an
ambiguous shortcut), [PR #17](https://github.com/btsouth/omaroll/pull/17)
(caption save depended on focus; a finished video left a blank stage) and
[PR #18](https://github.com/btsouth/omaroll/pull/18) (header count wording).
Version 1.6.0 carries all of it:

- [PR #11](https://github.com/btsouth/omaroll/pull/11): Tab and Shift+Tab cycle
  the section pills.
- [PR #12](https://github.com/btsouth/omaroll/pull/12): the metadata pass reads
  camera and lens as well as dates, Browse gains a Cameras section, and
  MediaDateIndex became MediaMetadataIndex with a new cache file.
- [PR #13](https://github.com/btsouth/omaroll/pull/13): star ratings with Alt+1
  to Alt+5, a Top rated sort and a minimum rating filter in Browse.
- [PR #14](https://github.com/btsouth/omaroll/pull/14): tags nest with a slash
  and files take a caption in the viewer that is searched with names and text.

The changelog lists them under 1.6.0. Semantic search and face
recognition were considered against Lightroom and Immich and deferred
(SBS-1139); do not add ML dependencies without a fresh decision.

## Next decisions and release gates

1. Keep new changes in separate focused PRs off main, following
   [RELEASING.md](../RELEASING.md) for the next tag.
2. Finish installed Omarchy acceptance (SBS-1121): file-manager selections,
   clipboard, drag/drop, scaling, window state and physical audio. Headless
   passes do not close this gate.
3. Continue performance work (SBS-1122): warm navigation, larger and mixed
   libraries, Wayland presentation, GPU memory and realistic regression budgets.
4. Desktop test the four unreleased features before the next tag, then start
   safe image corrections (SBS-1126): crop, rotate and resize with Save a
   copy, collision handling and a metadata/color policy. Reuse Omarchy helpers
   where they satisfy the workflow. Comparison and organization improvements
   follow in the [roadmap](ROADMAP.md).
5. Watch the Omarchy package submission for upstream feedback.

[Official package PR #295](https://github.com/omacom/omarchy-pkgs/pull/295)
remains open and was refreshed to v1.5.0 after the release. Earlier edge, rc
and stable package builds passed; upstream acceptance is pending. Repository inclusion, default
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
parent roadmap; SBS-1121 and SBS-1122 remain in progress, SBS-1138 and
SBS-1140 to SBS-1142 are done, and SBS-1126 is the next feature to start.
Notes under `build/roadmap-review/` are historical and ignored by Git. This
file is the committed handoff. No further implementation, merge or release
work is scheduled by closing this session.
