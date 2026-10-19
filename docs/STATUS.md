# Project status

## Latest release

[Omaroll 1.18.0](https://github.com/btsouth/omaroll/releases/tag/v1.18.0), dated
15 October 2026. See the [changelog](../CHANGELOG.md#1180) for what it includes.

The release provides x86_64 and aarch64 Arch packages, a source archive and
PKGBUILD, with checksums and signed build provenance. Installation and
verification commands are in the [README](../README.md#install-or-update).

## Current work

Version 1.18.0 adds frame stepping, names audio and subtitle tracks by their
language, and brings deep zoom for very large JPEGs to the library's preview
and the compare view, which also stops decoding large pictures whole. It was
built and tested against Omarchy stable (Qt 6.11.2) and current Arch (Qt 6.12)
in CI and in local containers.

Version 1.17.0 starts video playback without waiting for the thumbnail,
converts wide-gamut photos to sRGB, and zooms into very large JPEGs in the
viewer by decoding only the part on screen. Hardware video decoding stays the
default; the video startup note records the comparison with software
decoding. On Omarchy stable the first decoded frame moved from about 254 to
157 ms in a software-rendered container. Deep zoom memory was measured in an
isolated desktop on the graphics card.

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
The [video startup](performance/2026-10-14-video-startup/README.md) note records
first-frame timing and the hardware and software decoding comparison.
The [deep zoom](performance/2026-10-14-deep-zoom/README.md) note records area
decode times and memory for a 150-megapixel JPEG.

The [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295)
is still under review. Repository inclusion, default installation and MIME
associations are separate upstream decisions.

## Validation and remaining coverage

CI covers the core, library UI and quick viewer suites, rendered video with
OpenGL, dark and light themes, and narrow windows. A Debug job with
AddressSanitizer and UndefinedBehaviorSanitizer runs the whole test suite and
gates pull requests. Release validation also checks package installation,
upgrade, reinstall and removal.

Headless checks do not establish physical audio, multiple-monitor behavior,
fractional scaling or Wayland presentation performance. Use the
[desktop acceptance checklist](../tests/README.md#desktop-acceptance) for that
coverage, with disposable files for operations that change media.

PDF text selection is shipped. PDF links still need a dependency and licensing
decision before implementation; see the [roadmap](ROADMAP.md#open-questions).
