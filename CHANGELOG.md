# Changelog

## Unreleased

### Added

- Tab and Shift+Tab step through the sections, wrapping at either end.

## 1.5.0

### Added

- Open multiple selected files in order, including across folders and in an
  already-running window. Explicitly selected hidden files open without
  scanning their surrounding folders.
- Opt-in startup readiness tracing and a reproducible startup benchmark.

### Changed

- Video setup is deferred until a video is opened, reducing startup work for
  images and the library.
- Numbered filenames sort naturally, so image2 appears before image10.
- Copying extracted text is confirmed visually and through accessibility
  announcements.
- Real video rendering checks run under Mesa OpenGL in CI and release
  validation, and automated playback stays off physical audio devices,
  including native PipeWire.

### Fixed

- Thumbnail, PDF and matte image responses no longer risk a crash when a
  request is cancelled or finishes during fast scrolling or a tile size change.
- Tiles no longer keep a previous file's thumbnail after the window or tile
  size changes, which could open a different file than the one shown.

## 1.4.0

### Added

- Image previews now include actual-size viewing, horizontal and vertical
  flips, a checkerboard behind transparency, zoom up to 64 times the fitted
  size, and Space to pause animated GIF and WebP files.
- Video playback now includes standard Space, mute, volume, seek, speed, audio
  track, and subtitle controls. Double-clicking the video toggles fullscreen.
- SVG, icons, JPEG 2000, JXL, QOI, PSD, DDS, EXR, and TGA images can be scanned
  and opened directly when the packaged image plugins are installed.

### Changed

- Videos start with sound, play once, and preserve an intentional pause when
  the window is minimized and restored.

### Fixed

- Browse stays fully visible at the minimum window width.
- Closed matte previews no longer reload their previous file when the window
  changes size.
- Opening a PDF directly from a file manager no longer rejects it as an
  unsupported media file.
- Space in a video preview now pauses or resumes playback instead of launching
  the configured video action.

## 1.3.1

### Fixed

- Creating an album, tag, or smart collection from Browse now keeps keyboard
  focus in the naming field.

## 1.3.0

PDF support is the headline addition in this release. Omaroll can now treat
PDFs as first-class library items.

### Added

- Scan, thumbnail, preview, page through, organize, send, rename, and trash PDF
  documents. Rendering and page inspection use Poppler locally.
- Browse by month or day, plus quick views for today, this week, recently
  modified files, and files changed since the previous visit.
- Save the current filters and sort as a smart collection. Saved views update
  automatically as the library changes.
- Add reusable tags to any selection and browse tagged files without moving
  them. Tags follow in-app renames using the same identity checks as albums.
- Review visually similar pictures using a local perceptual comparison that
  tolerates resizing and JPEG recompression.
- Keep one file from an exact-duplicate set and move only its byte-for-byte
  identical copies to Trash after confirmation.

### Changed

- Browse now brings folders, albums, dates, tags, saved views, exact
  duplicates, and similar-picture review into one panel.
- Selection organization now includes both albums and tags.

## 1.2.0

### Added

- Extracted image text now opens in a selectable review sheet with preserved
  line breaks, Copy selection, Copy all, retry, and temporary corrections.
- The viewer action list supports Tab, arrow keys, Enter, Space, and
  accessibility activation. Shortcut tooltips now use the live bindings.
- QR actions appear only after a QR code is detected in the open image.
- OCR search results show the matching text when the filename did not match.

### Changed

- `Copy to clipboard` is now `Copy image`.
- Browse is now a bounded, searchable library panel with direct source,
  folder, album, duplicate, and add-folder controls. Folder labels are concise
  while the parent path and recursive item count remain visible as context.
  Search filters the choice model before rows are created, and reopening Browse
  returns to the active folder or album.
- Extract Text retries sparse screenshots once when the normal OCR pass finds
  almost no text. Results still share the same private cache.
- Extract Text takes priority over background OCR and stops its current pass
  when the review closes.
- Viewer actions keep keyboard focus when moving between files or closing a
  nested sheet. Extracted text receives focus as soon as it is ready.
- Large libraries use indexed path lookups and bounded image metadata batches.
  OCR queues start and stop without hashing every path.
- Libraries with more directories than the inotify safety cap get a periodic
  worker rescan, while ordinary watcher updates only change affected paths.

### Fixed

- First-run media date indexing publishes one settled update instead of
  repeatedly re-sorting the grid as each file is read. Visible thumbnails are
  reused, and folder changes no longer animate every tile into place.
- Background date enrichment and manual sort changes preserve the selected
  file and visible scroll anchor when rows move.
- Mass file removals use one bounded model refresh instead of thousands of
  synchronous row and folder-index updates.
- Late QR detection no longer moves keyboard focus to a different viewer
  action.

### Install

```bash
curl -fLO https://github.com/btsouth/omaroll/releases/download/v1.2.0/omaroll-1.2.0-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/btsouth/omaroll/releases/download/v1.2.0/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.2.0-1-x86_64.pkg.tar.zst
```

Every asset is covered by `SHA256SUMS` and a signed build attestation:
`gh attestation verify omaroll-1.2.0-1-x86_64.pkg.tar.zst --repo btsouth/omaroll`.

## 1.1.0

### Added

- Clear folder sources in Settings. Omaroll lists the Omarchy and XDG folders
  it detects automatically, combines roles that resolve to the same path, and
  marks unavailable paths. Added folders stay saved so removable storage can
  return later.
- Original media dates. General photos use EXIF DateTimeOriginal or
  DateTimeDigitized, and videos use their embedded creation time, so files
  copied into a watched folder still land on the day they were made. Omaroll
  reads one file at a time after discovery, caches both found and missing dates
  by file identity, and never overrides a timestamped capture filename.
- Find exact duplicates. Browse opens a read-only review that compares only
  same-size candidates, hashes them off the interface thread, keeps matching
  sets together, and updates when files change. It never removes anything.
- Save the current video frame. The viewer hands its exact playback position
  to `ffmpeg`, writes a timestamped PNG beside the recording, and tracks the
  result without changing the video.
- Rename in place. The sheet keeps the media extension fixed, refuses an
  existing filename, and carries favourites, hidden state, and album membership
  to the new path.
- Search inside pictures. Filename results appear immediately, then local
  Tesseract indexing adds screenshots and photos containing every search term.
  It starts only while search is active, runs one file at a time, pauses after
  the current file when search clears, and reuses a private, identity-checked
  cache pruned to 64 MB at startup. Progress is visible, and Settings can stop
  indexing and clear the text cache without touching media.
- Set as background. A picture can become the current Omarchy background from
  its action list without copying or moving it. Omaroll delegates the change to
  `omarchy-theme-bg-set` and keeps the viewer open.
- Media details. The viewer shows dimensions and duration, image format,
  camera and exposure data when present, plus video codec, frame rate, bitrate
  and audio. The details panel inspects only the file being viewed.
- Convert and resize choices. One sheet now exposes every image and video
  format and size supported by `omarchy-transcode`. It works on one file or a
  same-medium selection, tracks every output, and keeps every original intact.
- Send to a machine. Pick one of your own machines on the tailnet and the file,
  or the whole selection, goes over Taildrop through `omarchy-tailscale-send`.
  The picker lists only machines Tailscale says can take a file right now, and
  says why when there are none. The action is in the viewer and, when files are
  selected, in the header. It needs the `tailscale` package.
- Tile size. Ctrl and the wheel, or Ctrl with plus, minus and 0, make the grid
  tiles bigger or smaller between 160 and 480 pixels. The size is remembered.

### Fixed

- The matte picker no longer closes when you click a matte, an aspect or the
  padding button, and a right click on any sheet no longer opens the tile
  behind it. Every sheet now swallows clicks properly, the library under an
  open sheet or the viewer ignores input until it closes, and the click that
  closes the Browse, Sort or Album menu no longer also lands on what is under
  it.
- After using the Browse, Sort or Album menu the arrow keys work again at
  once, instead of being dead until a tile was clicked.
- On a narrow window the matte picker's aspect and padding buttons no longer
  run into Cancel and Copy and save; they drop onto their own line.
- A very tall screenshot made postable with a forced aspect kept its
  proportions in the preview but not in the saved file: the padding was not
  scaled with the canvas, so the capture came out as a thin strip and, at the
  largest padding, was cropped top and bottom. The saved matte now matches.
- An album entry is matched on inode and size together. Filesystems that hand
  a freed inode number to the next file could otherwise repoint an entry to an
  unrelated capture saved after a delete.
- Adding a file to an album at the name of a member whose file had gone
  replaces the unavailable entry instead of being refused.
- The theme no longer reloads and re-evaluates every colour binding each time
  something is copied to the clipboard. The shell writes its clipboard history
  beside the theme state, and that write used to count as a theme change.
- Theme switches are detected even if the filesystem misses its change
  notification during Omarchy's atomic directory replacement.
- If the single-instance socket cannot be created at all, Omaroll runs
  anyway rather than exiting silently with nothing on screen.
- A file whose name contains a `#`, a `?` or a literal `%` now gets a
  thumbnail and a matte preview. The path is percent-encoded once on the way
  into the image provider and decoded once on the way out.
- With a selection, `V`, `Ctrl+H`, `Y`, `S` and `Del` act on every checked
  file, as the header buttons and a drag already did, instead of only on the
  highlighted tile.
- Album reconciliation no longer stats every file in the library on the GUI
  thread after each rescan; the scanner gathers the identity off-thread. An
  album entry on btrfs, whose device numbers can change between boots, is no
  longer marked unavailable by the device number alone.
- Actions that leave the file where it is no longer close the viewer.
  Favourite, copy to clipboard, copy the text, scan QR code, send and show in
  files all run with the picture still open, and the result is shown inside
  the viewer instead of in the footer behind it. Trash, hide, the matte picker
  and the editors still close it as before.
- The favourite star in the viewer's sidebar follows the toggle while the
  viewer is open.
- Pressing a transcode action whose output already exists no longer freezes
  the window while ffprobe checks the file. The check runs in the background
  and the viewer opens on the file, or the run starts, when it answers.
- A text or QR recogniser that crashes after starting is reported once, with
  the tool's own last line, rather than as "could not start" followed by a
  second message.

### Install

```bash
curl -fLO https://github.com/btsouth/omaroll/releases/download/v1.1.0/omaroll-1.1.0-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/btsouth/omaroll/releases/download/v1.1.0/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.1.0-1-x86_64.pkg.tar.zst
```

Every asset is covered by `SHA256SUMS` and a signed build attestation:
`gh attestation verify omaroll-1.1.0-1-x86_64.pkg.tar.zst --repo btsouth/omaroll`.

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
curl -fLO https://github.com/btsouth/omaroll/releases/download/v1.0.3/omaroll-1.0.3-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/btsouth/omaroll/releases/download/v1.0.3/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.0.3-1-x86_64.pkg.tar.zst
```

Every asset is covered by `SHA256SUMS` and a signed build attestation:
`gh attestation verify omaroll-1.0.3-1-x86_64.pkg.tar.zst --repo btsouth/omaroll`.

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
curl -fLO https://github.com/btsouth/omaroll/releases/download/v1.0.2/omaroll-1.0.2-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/btsouth/omaroll/releases/download/v1.0.2/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.0.2-1-x86_64.pkg.tar.zst
```

Every asset is covered by `SHA256SUMS` and a signed build attestation:
`gh attestation verify omaroll-1.0.2-1-x86_64.pkg.tar.zst --repo btsouth/omaroll`.

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
curl -fLO https://github.com/btsouth/omaroll/releases/download/v1.0.1/omaroll-1.0.1-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/btsouth/omaroll/releases/download/v1.0.1/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.0.1-1-x86_64.pkg.tar.zst
```

Every asset is covered by `SHA256SUMS` and a signed build attestation:
`gh attestation verify omaroll-1.0.1-1-x86_64.pkg.tar.zst --repo btsouth/omaroll`.

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
curl -fLO https://github.com/btsouth/omaroll/releases/download/v1.0.0/omaroll-1.0.0-1-x86_64.pkg.tar.zst \
     -fLO https://github.com/btsouth/omaroll/releases/download/v1.0.0/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
sudo pacman -U ./omaroll-1.0.0-1-x86_64.pkg.tar.zst
```

Every asset below is covered by `SHA256SUMS` and by a signed build attestation.
`gh attestation verify omaroll-1.0.0-1-x86_64.pkg.tar.zst --repo btsouth/omaroll`
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
