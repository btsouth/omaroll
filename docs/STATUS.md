# Project status

## Latest release

[Omaroll 1.16.1](https://github.com/btsouth/omaroll/releases/tag/v1.16.1), dated
14 October 2026. See the [changelog](../CHANGELOG.md#1161) for what it includes.

The release provides x86_64 and aarch64 Arch packages, a source archive and
PKGBUILD, with checksums and signed build provenance. Installation and
verification commands are in the [README](../README.md#install-or-update).

## Current work

Version 1.16.1 opens photos larger than the screen from a copy the size of the
screen, preloads both neighbours as copies and returns freed pictures to the
system. Its memory and quality measurements ran in an isolated desktop that
renders in software, not on a physical graphics card.

Version 1.16.0 adds aarch64 packages. They are built and tested on Arch Linux
ARM in CI, the same way as x86_64, but have not been run on ARM hardware.
x86_64 packages are built against Omarchy's stable packages so they start on
the stable and rc channels; Omarchy has no aarch64 stable mirror to build
against.

Version 1.15.2 opens very large pictures, such as 150-megapixel scans, from a
copy the size of the screen and loads full resolution when zoomed in. Version
1.15.0 added an optional library folder sidebar, viewer looping, audio track
and subtitle timing controls, fit modes and shortcut help. Physical desktop
acceptance on Omarchy covered viewer tiling, video audio, media keys and
clipboard paste into other apps in an earlier release; 1.15.2 and later were
checked in an isolated Omarchy desktop, not on physical hardware.

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
The [screen-sized copies](performance/2026-10-14-screen-copies/README.md) record
measures memory, startup and picture quality for photos larger than the screen.

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
