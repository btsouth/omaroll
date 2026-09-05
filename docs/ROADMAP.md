# Roadmap

Omaroll should cover everyday media viewing and organization on Omarchy:
open files quickly, find them again, make a quick correction, and share the
result. Discovery stays read-only and core use stays offline.

## Reliability and package adoption

- Run real animated-image, video, audio and subtitle checks in ordinary CI.
- Verify installed behavior on Omarchy: file-manager opening, scaling,
  clipboard, drag/drop, window state and physical audio.
- Measure current startup time, library responsiveness and resource use.
- Maintain the [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295).

## Complete image workflows

- Open multiple selected files.
- Crop, rotate and resize with Save a copy and safe collision handling.
- Copy a selected region and compare images with synchronized zoom.
- Verify orientation, color profiles, transparency and large-image behavior.

## Trustworthy organization

- Back up and restore albums, tags, favorites and smart collections.
- Rename albums and tags without losing membership.
- Recover predictably from disconnected drives and changed file locations.
- Make organization changes reversible and duplicate review easier.

## Video and PDF depth

- Remember playback preferences and offer resumable playback.
- Load external subtitles and expose clear track choices.
- Add PDF text selection, search, page navigation, links and printing.

Each phase ships in useful increments. Repository inclusion, installation by
default and MIME defaults are separate upstream decisions. Image defaults are
the first adoption target; video and PDF need their own acceptance checks.
Advanced editing remains available through Omarchy's existing tools.

Report concrete missing workflows and reproducible problems in
[GitHub issues](https://github.com/btsouth/omaroll/issues).
