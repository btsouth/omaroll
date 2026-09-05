# Project status and handoff

Snapshot: 5 September 2026 UTC, after PR #8 merged. Check the linked PRs and release before resuming.

## Released

[v1.4.0](https://github.com/btsouth/omaroll/releases/tag/v1.4.0) remains the latest
public release. 1.5.0 is prepared but not tagged or published.
The viewer supports still and animated images, embedded video controls, PDF
paging, OCR/QR, albums, tags, saved collections and duplicate review. The
README describes the supported formats and workflows.

## Prepared: 1.5.0, not yet tagged

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

CodeRabbit reviewed PR #6 with one minor finding, which was fixed before the
squash merge. Core, UI, sanitizer and OpenGL checks passed locally and in CI.
The intermittent SIGSEGV in `omaroll_ui_tests` seen once on 2026-09-04 was
traced through its core dump to the response lifetime race fixed in PR #8.

[Local measurements](performance/2026-09-05-startup/README.md) record medians
of 949.9 to 257.2 ms for an image-ready submitted frame and 1181.2 to 471.9 ms
for a ready grid. These are offscreen NVIDIA measurements from main entry,
not compositor presentation or general performance guarantees.

## Next decisions and release gates

1. Try the 1.5.0 candidate on the real Omarchy desktop, then tag `v1.5.0`
   following [RELEASING.md](../RELEASING.md). Keep new changes in separate
   focused PRs.
2. Finish installed Omarchy acceptance (SBS-1121): file-manager selections,
   clipboard, drag/drop, scaling, window state and physical audio. Headless
   passes do not close this gate.
3. Continue performance work (SBS-1122): warm navigation, larger and mixed
   libraries, Wayland presentation, GPU memory and realistic regression budgets.
4. Start safe image corrections (SBS-1126): crop, rotate and resize with Save a
   copy, collision handling and a metadata/color policy. Reuse Omarchy helpers
   where they satisfy the workflow. Comparison and organization improvements
   follow in the [roadmap](ROADMAP.md).
5. After 1.5.0 ships, refresh the Omarchy package submission to it.

[Official package PR #295](https://github.com/omacom/omarchy-pkgs/pull/295)
remains open and targets v1.4.0. Earlier edge, rc and stable package builds
passed; upstream acceptance is pending. Repository inclusion, default
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
parent roadmap; SBS-1121 and SBS-1122 remain in progress, and SBS-1126 is the
next feature to start.
Notes under `build/roadmap-review/` are historical and ignored by Git. This
file is the committed handoff. No further implementation, merge or release
work is scheduled by closing this session.
