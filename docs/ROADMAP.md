# Roadmap

Omaroll should cover everyday media viewing and organization on Omarchy:
open files quickly, find them again, make a quick correction, and share the
result. Discovery stays read-only and core use stays offline.

See [current status and handoff](STATUS.md) for release boundaries and evidence.

## Reliability and package adoption

- Maintain the animated-image, video-track, subtitle and rendered-pixel checks
  already running in CI. Local tests stay audio-isolated.
- Verify installed behavior on Omarchy: file-manager opening, scaling,
  clipboard, drag/drop, window state and physical audio.
- Extend recorded startup and 10k/50k library baselines with warm navigation,
  larger/mixed libraries, compositor presentation and regression budgets.
- Maintain the [Omarchy package submission](https://github.com/omacom/omarchy-pkgs/pull/295).

## Complete image workflows

- Crop, rotate and resize with Save a copy and safe collision handling. Done:
  the viewer's Crop, rotate, resize action writes `<name>-edited.<ext>` beside
  the original, preserving the ICC profile and baking in EXIF orientation.
- Copy a selected region and compare images with synchronized zoom. Compare is
  done: the Compare action (K) shows the checked selection or the open file's
  duplicate/similar set with one shared zoom and pan. Region copy remains.
- Verify orientation, color profiles, transparency and large-image behavior.

## Trustworthy organization

- Back up and restore albums, tags, favorites and smart collections. Done:
  a versioned JSON backup covers albums, tags, favourites, hidden files,
  ratings, captions and saved views, validated before any change on restore.
- Rename albums and tags without losing membership. Done: renaming moves the
  membership with the name, and a nested tag rename carries its children.
- Recover predictably from disconnected drives and changed file locations.
  Partial: favourites, hidden flags, ratings and captions now follow an
  external move or rename by inode or content fingerprint, matching what
  albums and tags already did.
- Make organization changes reversible and duplicate review easier.

## Video and PDF depth

- Remember playback preferences and offer resumable playback. Done: volume and
  mute persist, and a reopened video offers Resume or Start over from its
  saved spot.
- Load external subtitles and expose clear track choices. Done: a `.srt` or
  `.vtt` beside the video joins the CC cycle with a language label and renders
  over the video; embedded named tracks keep working.
- Add PDF text selection, search, page navigation, links and printing.

Each phase ships in useful increments. Repository inclusion, installation by
default and MIME defaults are separate upstream decisions. Image defaults are
the first adoption target; video and PDF need their own acceptance checks.
Advanced editing remains available through Omarchy's existing tools.

Report concrete missing workflows and reproducible problems in
[GitHub issues](https://github.com/btsouth/omaroll/issues).
