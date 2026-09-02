# Changelog

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
