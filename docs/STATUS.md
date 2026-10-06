# Project status

## Latest release

[Omaroll 1.15.1](https://github.com/btsouth/omaroll/releases/tag/v1.15.1), dated
6 October 2026. See the [changelog](../CHANGELOG.md#1151) for what it includes.

The release provides an x86_64 Arch package, source archive and PKGBUILD, with
checksums and signed build provenance. Installation and verification commands
are in the [README](../README.md#install-or-update).

## Current work

Version 1.15.1 adds fill mode (Shift+W) to the quick viewer, which stays on
while stepping through a folder. Version 1.15.0 added an optional library
folder sidebar, viewer looping, audio track and subtitle timing controls, fit
modes and shortcut help, and grid filenames, pointer zoom, path copying and a resizable preview. A second quick
viewer maps directly into its Hyprland tile without blocking, metadata appears
during indexing, and file actions, restores and single-instance hand-offs are
safer. Physical desktop acceptance on Omarchy covered viewer tiling, video
audio, media keys and clipboard paste into other apps.

[#80](https://github.com/btsouth/omaroll/issues/80) stays open for the
reporter's version and file-manager setup.

Use the [roadmap](ROADMAP.md) for future work and open decisions.

The [mixed-media baseline](performance/2026-10-02-mixed-media/README.md) records
startup, whole-library jumps, viewer navigation and the preloading comparison.
Its generated fixtures and headless measurements have explicit limits.
The [bounded-work follow-up](performance/2026-10-03-bounded-media/README.md)
records neighboring-image admission, thumbnail cancellation and repeated
navigation measurements.
The [gallery loading follow-up](performance/2026-10-03-gallery-loading/README.md)
records sizing and recycling fixes, continuous input, and a discarded scheduling
policy that regressed cached navigation.

The [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295)
is still under review. Repository inclusion, default installation and MIME
associations are separate upstream decisions.

## Validation and remaining coverage

CI covers the core, library UI and quick viewer suites, rendered video with
OpenGL, dark and light themes, and narrow windows. Release validation also
checks package installation, upgrade, reinstall and removal.

Headless checks do not establish physical audio, multiple-monitor behavior,
fractional scaling or Wayland presentation performance. Use the
[desktop acceptance checklist](../tests/README.md#desktop-acceptance) for that
coverage, with disposable files for operations that change media.

PDF text selection is shipped. PDF links still need a dependency and licensing
decision before implementation; see the [roadmap](ROADMAP.md#open-questions).
