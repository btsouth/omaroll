# Project status

## Latest release

[Omaroll 1.11.0](https://github.com/btsouth/omaroll/releases/tag/v1.11.0), dated
2 October 2026. See the [changelog](../CHANGELOG.md#1110) for what it includes.

The release provides an x86_64 Arch package, source archive and PKGBUILD, with
checksums and signed build provenance. Installation and verification commands
are in the [README](../README.md#install-or-update).

## Current work

Use the [open pull requests](https://github.com/btsouth/omaroll/pulls) for work
under review, and the [roadmap](ROADMAP.md) for scope and future decisions.
Changes for the next release should remain in focused pull requests; the
published tag identifies the 1.11.0 source.

The [mixed-media baseline](performance/2026-10-02-mixed-media/README.md) records
startup, whole-library jumps, viewer navigation and the preloading comparison.
Its generated fixtures and headless measurements have explicit limits.

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
