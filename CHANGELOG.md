# Changelog

## 1.0.3

### Fixed

- Transcode actions always show a result. A running transcode pins a
  "Making ..." line in the footer and selects the finished file in the grid,
  and pressing an action whose output already exists opens the viewer on it.
- Existing outputs are verified with ffprobe before being trusted, so an
  empty or truncated file left by an interrupted run is cleared and remade
  instead of blocking every retry as "already done".
- A `-720p.gif`, `-1080p.mp4` or `-4k.mp4` renders its thumbnail from the
  source video beside it, so a recording and its conversions show identical
  tiles instead of three different frames of the same content.
- A recording's tile returns to its resting frame when a hover scrub ends,
  instead of parking on whatever frame the hover left it at.
- An animated GIF with no source beside it thumbnails at the same percent-in
  moment as a video, rather than always its first frame.

The thumbnail cache regenerates lazily on first view after upgrading.

### Install

```bash
curl -fLO https://github.com/tsouth89/omaroll/releases/download/v1.0.3/omaroll-1.0.3-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/tsouth89/omaroll/releases/download/v1.0.3/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.0.3-1-x86_64.pkg.tar.zst
```

Every asset is covered by `SHA256SUMS` and a signed build attestation:
`gh attestation verify omaroll-1.0.3-1-x86_64.pkg.tar.zst --repo tsouth89/omaroll`.

## 1.0.2

Clip to GIF, Resize to 1080p and Convert to JPEG used to be fire-and-forget:
the new file blended into the library with no word about where it went, a
transcode's output sat in the grid as a broken zero-byte tile while ffmpeg
worked, and if the run died the tile stayed broken forever with no
explanation. Transcodes are now followed to the end.

### Fixed

- **You can see where the file went.** When a transcode finishes, the library
  rescans, the status line says the new file was saved beside the original,
  and the grid selects and scrolls to it. If the current filter would hide
  it, the view clears to show it, the same way "Open with Omaroll" does;
  otherwise your filters are left alone.
- **No more broken tiles mid-transcode.** The output file is held out of the
  library until the tool finishes, so the zero-byte in-progress file never
  appears as a broken entry, and its thumbnail is only made once the file is
  complete.
- **Failures are cleaned up and explained.** A transcode that dies leaves no
  partial file behind, and the tool's own last error line is quoted in the
  status message instead of silence.
- **No more false "already done".** Running the same transcode again while
  one is in flight says "Still working on ..." instead of mistaking the
  half-written file for a finished one.
- **Stale entries fix themselves.** Finished transcodes are rescanned even
  though writing into an existing file fires no directory event, which was
  why a thumbnail made from a half-written file used to stay broken until
  restart.

One behaviour change: a transcode is now cancelled if you close Omaroll while
it is running, rather than continuing unwatched in the background.

### Install

```bash
curl -fLO https://github.com/tsouth89/omaroll/releases/download/v1.0.2/omaroll-1.0.2-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/tsouth89/omaroll/releases/download/v1.0.2/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.0.2-1-x86_64.pkg.tar.zst
```

Every asset is covered by `SHA256SUMS` and a signed build attestation:
`gh attestation verify omaroll-1.0.2-1-x86_64.pkg.tar.zst --repo tsouth89/omaroll`.

## 1.0.1

Omaroll now resolves theme colors the way Omarchy itself does. Before this
release, only themes with a fully semantic `colors.toml` followed your theme;
anything else quietly fell back to Omaroll's built-in green look.

### Fixed

- **Compact terminal palettes follow the theme.** A `colors.toml` that defines
  only `background`, `foreground` and `color0`..`color15` (the format many
  third-party and Ghostty-derived themes use) now resolves through the same
  alias cascade as `omarchy-theme-color`: ANSI names map to semantic names,
  `muted`, `selection` and the foreground variants follow the same fallback
  chains, missing dark and darker background shades are derived with the same
  25% and 50% black mixes, and light or dark mode is detected with the same
  precedence and luminance threshold.
- **Accent falls back to the terminal blue.** When a theme defines no `accent`,
  Omaroll takes `color4`, exactly like the Omarchy shell, instead of showing
  the built-in green accent.
- **Legacy names and the Omarchy 3 layout work again.** Themes using the old
  short names (`bg`, `fg`, `dark_bg`, ...) resolve correctly, and a theme
  installed under `~/.config/omarchy/current` is found when the Omarchy 4
  state root has none.
- **The state root matches Omarchy.** Omaroll reads the fixed
  `~/.local/state/omarchy/current` path that Omarchy's own scripts use, rather
  than honouring `XDG_STATE_HOME` when Omarchy does not.
- **Machine-level launcher overrides apply.** A `[launcher]` `background` or
  `background-alpha` in `~/.config/omarchy/shell.toml` overrides the theme's
  values, updates live when the file changes and reverts when it is removed,
  matching the Omarchy shell's "user keys win" rule.
- **Theme switches are picked up reliably.** The atomic directory replacement
  `omarchy-theme-set` performs (staging `next-theme`, then renaming it into
  place) triggers a reload, as does creating or removing a `light.mode`
  marker.

Six new regression tests cover the cascade, the accent fallback, the legacy
locations and the live override behaviour, with resolved values verified
byte-for-byte against `omarchy-theme-color` output.

### Install

```bash
curl -fLO https://github.com/tsouth89/omaroll/releases/download/v1.0.1/omaroll-1.0.1-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/tsouth89/omaroll/releases/download/v1.0.1/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.0.1-1-x86_64.pkg.tar.zst
```

Every asset is covered by `SHA256SUMS` and a signed build attestation:
`gh attestation verify omaroll-1.0.1-1-x86_64.pkg.tar.zst --repo tsouth89/omaroll`.

## 1.0.0

Omaroll is a fast image and video viewer that turns your media folders into a
library. Built for Omarchy, as an independent community project.

Omarchy ships a very good capture stack and then saves everything to folders.
Omaroll is the part that comes after: finding the thing again, and doing the
obvious next thing with it.

### Highlights

- **One library for everything you capture.** Screenshots, recordings,
  pictures, videos and downloads, grouped by day, newest first. Add any other
  folder from Settings and switch between folders from the library bar.
- **Knows what each file is.** Omarchy stamps its captures with a predictable
  name, so a screenshot is a Screenshot even when it lives next to every other
  image in `~/Pictures`.
- **A viewer with every action beside the picture.** Enter opens a capture
  large. Images zoom, pan, rotate and animate. Recordings preview muted with a
  scrub bar and a sound toggle. F11 for fullscreen, F5 for a slideshow of any
  folder, album, search or filtered view.
- **The tools you already have, one key from the file.** T sends a recording
  to omacut, A sends a screenshot to tensaku or `$OMARCHY_SCREENSHOT_EDITOR`,
  and the action list runs omarchy-transcode, mpv, tesseract, zbarimg, Pinta,
  imv, LocalSend and Nautilus. A tool that is not installed is shown greyed
  with the package to install, not hidden.
- **Make it postable.** The one thing Omaroll does natively: six finished
  backgrounds derived from the screenshot's own dominant colour, the same six
  every time for the same file. Pick one and it is on your clipboard and saved
  beside the original.
- **Albums that never move or copy media.** Select files, add them to a named
  album, browse it beside your folders. If a member is renamed or moved inside
  the library the entry is repaired by file and content identity. A file moved
  elsewhere shows as unavailable rather than being matched to the wrong copy.
- **Drags out as the real file.** Pull a thumbnail into Discord, a browser
  upload or a file manager. Select several and they go together.
- **Follows your theme.** Reads the active Omarchy palette, font, corner
  radius and launcher transparency, then cross-fades when the theme changes.
- **Keyboard first.** Arrows and hjkl in the grid, single letters for every
  action, `/` to search, `1` to `7` to jump between sections.

### Formats

Images: PNG, JPEG, WebP, animated GIF and WebP, BMP, AVIF, HEIC/HEIF and TIFF.
Videos: MP4, M4V, MKV, WebM, MOV, AVI, MPEG, WMV, FLV, Ogg video, 3GP and
MTS/M2TS, played through Qt's FFmpeg backend.

### Install

Requires Omarchy, or Arch with Qt 6.8 or newer.

```bash
curl -fLO https://github.com/tsouth89/omaroll/releases/download/v1.0.0/omaroll-1.0.0-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/tsouth89/omaroll/releases/download/v1.0.0/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.0.0-1-x86_64.pkg.tar.zst
```

Every asset below is covered by `SHA256SUMS` and by a signed build attestation.
`gh attestation verify omaroll-1.0.0-1-x86_64.pkg.tar.zst --repo tsouth89/omaroll`
confirms the package was built by this repository's release workflow from the
tagged commit.

Omarchy dims every window slightly by default, which washes out thumbnails.
Omaroll opts out the same way mpv, imv and Pinta do. The rule is installed at
`/usr/share/omaroll/hypr/omaroll.lua`; copy it into `~/.config/hypr/` and
require it, as the README describes.

### Try it without your own files

`omaroll --demo` browses a fictional library in a temporary folder. Every
screenshot in the repository was made this way.

### Guarantees

- Omaroll never moves, renames or rewrites a file it finds. Tools it hands off
  to write their results beside the original.
- Trash goes through the desktop trash, with a confirm, never `unlink`.
- Local only. No network, no telemetry, no accounts.
- Thumbnails are rendered at your device pixel ratio and cached under
  `~/.cache/omaroll`, bounded at 256MB by default and pruned least recently
  used. Settings live in `~/.config/omaroll`. Both are safe to delete.
- MIT licensed.
