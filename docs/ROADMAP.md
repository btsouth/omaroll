# Roadmap

Omaroll should cover everyday media viewing and organization on Omarchy:
open files quickly, find them again, make a quick correction, and share the
result. Discovery stays read-only and core use stays offline.

See [project status](STATUS.md) for the latest release and current work.

## Current priorities

- Refine the quick viewer and library together: navigation, search, selection,
  clear feedback, and controls that work in narrow windows and both themes.
- Use the [continuous-input comparison](performance/2026-10-03-gallery-loading/README.md)
  when evaluating gallery scheduling. Preserve cached navigation and distinguish
  submitted frames from desktop presentation and physical audio.
- Improve desktop integration and reliability from reproducible problems and
  user feedback. Preserve originals and keep specialist editing in the
  existing tools.
- Very large pictures in formats other than JPEG still decode whole when
  zoomed in, since their decoders cannot read part of a picture.

Shipped work is in the [changelog](../CHANGELOG.md).

## Open questions

- **PDF links.** On-page text selection is delivered, since `pdftotext -bbox`
  already reports a box per word. Links are what remains, and the two options
  are now measured rather than assumed. QtPdf provides link, bookmark and
  search models, but on Arch it ships inside `qt6-webengine` (282 MiB
  installed on the development desktop) and its QML module is not installed
  there, so it would be used through its C++ API. Linking `poppler-qt6`
  instead costs about 270 KiB and gives links, word boxes, an outline and
  in-process page rendering, but its bindings are GPL. Review the dependency
  footprint and license compatibility with Omaroll before choosing an integration.
- **Physical desktop gates.** Installed field acceptance and Wayland/GPU
  presentation numbers and regression budgets need the real Omarchy session.
  Headless runs cannot close them.

## Out of scope

Considered for 1.8.0 and left out; revisit only with a fresh decision:

- **XMP sidecars.** The versioned JSON backup already covers portability, and
  writing sidecars would go against the read-only organization policy.
- **Video chapters, play queue and loop.** The viewer plays video; these
  additional controls are outside the current scope.
- **Subtitle styling and a separate track picker.** Tracks are already named
  and the default rendering is readable.
- **Offline Places from EXIF GPS.** Needs a bundled geodata table.
- **PDF selectable-text panel.** Copy page text already covers it.
- **A duplicate/similar review flow.** Provided by the filters, Keep selected
  and compare.
- **Target-monitor fullscreen.** Hyprland owns window placement.

## Reliability, scale and adoption

- Maintain the animated-image, video-track, subtitle and rendered-pixel checks
  already running in CI. Local tests stay audio-isolated.
- Extend the benchmarks with warm navigation and 50k mixed libraries where
  they can run headless; the Wayland presentation and GPU memory numbers need
  the desktop.
- The [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295)
  is under review.

Repository inclusion, installation by default and MIME defaults are separate
upstream decisions. Advanced editing remains available through Omarchy's
existing tools.

Report concrete missing workflows and reproducible problems in
[GitHub issues](https://github.com/btsouth/omaroll/issues).
