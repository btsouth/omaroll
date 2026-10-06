# Changelog

## Unreleased

### Added

- `Shift+W` or Fill window in the quick viewer menu crops a picture to cover
  the window, centred, so tiled viewers have no empty bands. Press it again to
  fit the whole picture.

## 1.15.0

A second quick viewer opens straight into its tile without stalling the app,
large libraries show metadata and thumbnails sooner, and the library gains an
optional folder sidebar. The quick viewer adds looping, audio track and
subtitle timing controls, and file actions, restores and hand-offs are safer.

### Added

- An optional folder sidebar in the library, toggled with F9 or the Folders
  button. It shows pinned folders and a tree of library folders with counts,
  and its visibility and width are remembered.

- An optional Show filenames in grid setting keeps elided names on thumbnails,
  with the full name on hover or keyboard focus. It is off by default.
- Copy path and Copy file name in library menus and previews. `Ctrl+Shift+C`
  copies the opened entry's path as text, including symlinks.
- Resize the library preview by dragging its edges or corner. Its last size
  is remembered and fits within the current window.

- Loop videos in the quick viewer with the menu or Shift+L, cycle audio tracks
  with Shift+A, and adjust sidecar subtitle timing with Z and X.
- An optional Remember playback speed setting applies the last chosen quick
  viewer speed to newly opened videos. It is off by default.
- Copy paths and file names from the quick viewer and its details card.
- Fit pictures to the width or without enlarging, with a saved enlargement setting.
- Pan zoomed pictures with Shift+arrows and see viewer shortcuts with ? or F1.

### Changed

- A second quick viewer on Hyprland asks the compositor without blocking and
  maps directly into its tile. Closing a viewer never rearranges the others.
- Media metadata appears while indexing instead of at the end, and album and
  tag checks on large libraries are faster.
- Search waits for a short pause in typing; Enter, saved views and opening a
  result apply it at once.
- Thumbnails refresh when a file is replaced, even with its old modification
  time, and existing cached thumbnails are kept after the upgrade.
- OCR results are kept per language setting. Existing results carry over
  without running text recognition again.
- PDF rendering, text extraction and search have size and time limits and run
  off the main thread.

### Fixed

- Opening files from outside reliably reaches the running instance, with a
  clear error when it cannot, and simultaneous starts no longer race.
- Quick viewer Trash, slideshows and renames stay on the file being shown,
  including symlinks and files renamed by other apps.
- Video sound and media keys follow the active video across windows, and
  resume positions survive renames but not replacements.
- Library exports, corrections, renames and Trash act on the files chosen
  when the prompt opened and never replace an existing file.
- Restoring a backup validates it first and rolls back on failure. Save
  errors are shown, and albums and tags follow edited files.
- Clipboard actions report failure when the helper fails, and conversions
  stop cleanly when Omaroll quits.

- Wheel and pinch zoom in the library preview and Compare keep the point under
  the pointer fixed.

- Icon buttons support Tab, Enter and Space, and the library preview has an
  `Alt+C` caption shortcut and keyboard access to technical details.
- Library Undo reacts to the first mark. Failed Trash targets and other checked
  files stay selected after file actions.
- Grid filenames appear on hover or keyboard focus. Failed previews have a
  distinct message, long toolbar labels fit narrow windows, and Settings
  scrolls focused controls into view.
- Picture exports bake orientation and preserve color profiles. Background
  copies convert tagged pictures to sRGB, and batch JPEG turns use the same
  lossless path as single-picture corrections.
- Animated pictures clearly offer a still first-frame copy. Correction sheets
  ignore earlier jobs after reopening, keep crop presets consistent when
  rotating, and list failed batch files with a retry action.
- Taildrop reports send failures and completion without replacing failed starts
  with a sending message.

Thanks to @dyedfox for the folder sidebar, grid filenames, pointer zoom, copy
path and resizable preview requests, and to @ykrytsun-ait for also requesting
the last four.

[Install or update](https://github.com/btsouth/omaroll#install-or-update).

## 1.14.0

Photos and videos open faster, and videos work with media keys and the shell's
player controls. This release also brings the application chooser and more
navigation shortcuts to both viewers.

### Added

- Media keys and the desktop player panel control Omaroll videos through MPRIS,
  including play/pause and seeking. Next/previous controls are not offered.
- Additional video types sent by Omarchy's file associations open in Omaroll,
  including 3G2, ASF and OGM. Audio-only Ogg associations stay unchanged.
- `Shift+Delete` permanently deletes the current file or checked library files.
  Confirmation is on by default, with an explicit opt-out in Settings.
  Regular `Delete` still uses recoverable Trash.
- Open with offers the desktop application chooser for the current file,
  without changing MIME defaults. The menu and `Ctrl+O` work in both viewers
  and the library grid.
- Show in file manager selects the current file through the standard
  file-manager interface of the configured folder handler, falling back to
  opening its containing folder with `xdg-open` when selection is unavailable.
- Mouse back and forward buttons navigate files in both viewers.
- `1` toggles fit and actual size in both image viewers; from a custom zoom,
  it still goes to actual size first. `0` still fits.

### Changed

- Startup uses the desktop portal theme instead of loading GTK3.
- Opening a video shows its poster while the player starts. Linux playback
  avoids Qt's broad hardware-device scan, uses guarded CUDA/VAAPI discovery,
  and retains software fallback and explicit user overrides.

Thanks to @dyedfox for the viewer shortcuts, Open with, file-manager and
permanent-delete requests.

[Install or update](https://github.com/btsouth/omaroll#install-or-update).

## 1.13.0

### Added

- Camera raws open in the viewer and the library: Sony ARW, Canon CR2 and CR3,
  DNG, Nikon NEF, Fujifilm RAF, Olympus ORF, Panasonic RW2, Pentax PEF, Samsung
  SRW and the other formats LibRaw reads, through the kimageformats raw plugin
  when `libraw` is installed. Thumbnails and previews use the camera's embedded
  JPEG, turned upright, or a half-size decode when there is none, so stepping
  through a folder of raws is as quick as JPEGs. The viewer demosaics the raw
  itself only when that preview would be
  shown larger than it is. Dates, camera, lens and exposure come from the
  raw's own headers.
- Actions whose tool cannot open a raw (annotate, text and QR recognition,
  Pinta, imv, background, print and corrections) are not offered for one.
  Convert · resize still makes a JPEG or PNG beside it. Raws are never written.
- The Arch package depends on `libraw`, so raws open without extra setup.
- Camera RAW tiles show their format, and Develop opens an installed darktable,
  RawTherapee or ART. The viewer shortcut is `D`.
- The viewer can show a filmstrip of the folder above its controls. B or the
  menu turns it on, a click on a thumbnail opens that file, and the choice is
  kept.

- Matching RAW/JPEG files are grouped, with companion switching and format
  badges. Both originals and explicitly opened files remain accessible.
- Save a preferred RAW editor, choose one each time, or use a custom command.
- Pin folders for quick access.
- Open existing images in Omaframe for annotation and frames.
- Right-click files and selections for grouped actions, including native viewing,
  albums and tags, file copying, and printing supported selections.

### Changed

- New profiles use built-in viewing for Space. Upgrades keep their preferences.
- Make it postable is now Add background, available without Omaframe.
- QR copying appears only after detecting a code in the inspected image.
- Open containing folder uses your default file manager.

### Fixed

- Compare uses the captured image selection and appears only when usable.
  Preview actions exclude unrelated selections and unsupported batch operations.
- Menu shortcuts use captured targets. Slideshow menus hold their target until
  dismissed and resume correctly, including skipped videos.
- Background saving and copying report separate results. Retry copy reuses the
  saved image without recomposing or making another file.
- Image copying and print submission wait for the helper result before reporting
  success. Failed actions show an error.
- Editing a custom command selects it. Canceling or failing to launch keeps the
  saved command. Shell command strings are refused; wrapper
  scripts can receive the path as a separate argument.

## 1.12.0

### Changed

- Installation instructions use the signed package repository so Omaroll can
  update with the rest of the system. Manual release downloads remain available.

### Fixed

- QR detection stops safely when its media library or the application closes.
- Gallery thumbnails wait for valid dimensions and hide a previous file's
  retained pixels while a replacement loads. Viewport checks account for
  files inserted or removed above a scrolled grid.
- Installed documentation links resolve to the repository and packaged license.
  Arch dependencies explicitly include FFmpeg for frame saving and probing.
- The viewer bounds neighboring photo preloads while keeping originals at full
  resolution. Canceled thumbnails stop obsolete helper work, and queued
  requests finish during shutdown.
- Gallery photo and document details avoid an unused video-poster decode.
  Gallery and viewer video posters apply display scaling once.
- The gallery restores keyboard navigation after empty search results and
  keeps the search field in sync with saved views and files opened from outside.
- Browse keeps search and folder choices visible in small windows with many
  source folders. Folder-only shuffle stays in the opened file's folder.
- The viewer reloads files saved in place, preserves zoom and rotation when
  other files arrive, and uses the same identity for symlinks as the library.
- Video scrubbing and Home/End override saved resume positions. Shuffled
  slideshows reliably find pictures when videos are excluded.
- Organization survives disconnected folders. Backups retain the identities
  needed to recover marks after files move, and rename recovery preserves
  unrelated ratings and captions.
- Repeated conversion requests share one output check. A helper failure after
  writing media keeps the nonempty output beside the original for review.
- Reused filesystem identities no longer transfer marks to unrelated files.
  Recovery checks reuse versioned fingerprints, and legacy backups remain
  importable when content fingerprints are unavailable.
- Failed removal of invalid conversion outputs reports an error instead of
  treating the file as a completed conversion.

## 1.11.0

### Added

- Portrait crop presets: 3:4, 2:3 and 9:16 sit beside their landscape
  partners in Crop, rotate, resize. A held landscape ratio could not be turned
  upright, so a portrait, a poster or a phone-screen crop had to be drawn by
  hand. Thanks to @diogochaves ([#51](https://github.com/btsouth/omaroll/pull/51)).

### Changed

- The Crop, rotate, resize controls are grouped into labelled Crop, Rotate and
  Resize rows instead of one strip that wrapped wherever it ran out of room.
  On a wide sheet the output size ends the Crop row and Reset, Cancel, Copy
  region and Save a copy end the Resize row; a narrow one gives them a line of
  their own. The smallest window drops the row labels and packs the groups onto
  shared lines with a rule between them, so the preview keeps its height. Full
  frame is gone: it did exactly what Free does. Thanks to @diogochaves
  ([#51](https://github.com/btsouth/omaroll/pull/51)).

### Fixed

- The Sub − and Sub + timing buttons for a sidecar subtitle sat on the video
  clock. The clock now stands to their left, and they leave with the CC button
  on a narrow window, or before they would squeeze the scrub bar below its
  minimum. Their offset label sits to their left, so a click no longer moves
  or hides them. Thanks to @diogochaves ([#50](https://github.com/btsouth/omaroll/pull/50)).

## 1.10.0

### Added

- Each picture or video opened from outside gets its own viewer, so several
  can be open at once. Opening a file that is already up brings its viewer
  forward. On Hyprland, a second viewer on a workspace tiles with the ones
  there instead of floating on top of them.
- With several videos open, only the viewer used last is heard. The others
  keep playing muted, and the saved mute setting is unchanged.

### Fixed

- Images and videos without filename extensions open using bounded content
  detection. Folder navigation, library previews, animations and thumbnails
  use the same detected format; unrelated files remain excluded.

## 1.9.1

### Changed

- Gallery previews give the picture more room, with a compact filename header
  and an inspector available on demand. Ratings, captions and primary actions
  stay easy to reach; technical details expand when needed, and secondary
  actions live in an overflow menu.
- Video controls keep a useful seek target with the inspector open in a narrow
  window. The slideshow button appears for pictures and active slideshows,
  leaving ordinary video playback with its own controls.
- The README demo and preview screenshot show the polished gallery.

### Fixed

- Resume and Restart choices sit below the video and wrap when space is tight.
  A saved position holds playback until a choice is made; starting playback,
  seeking or a slideshow dismisses the offer.
- Caption edits are saved before navigation, and nested actions return keyboard
  focus to the preview.

## 1.9.0

### Added

- A quick viewer. A picture or video opened from the file manager fills a
  window of its own, with the name, the other files and a menu that fade in
  when the pointer moves. The arrow keys step through the folder in name
  order, the next picture is decoded ahead, the wheel and a pinch zoom
  smoothly around the pointer, and a zoomed picture moves with a drag. Pictures get one bar of the usual controls (zoom, actual
  size, previous, slideshow, next, rotate and delete) and videos a thin bar of
  their own. The window opens at most of the screen's size, the same for
  every file, and each picture is fitted inside it, a small one scaled up. Nothing is scanned to open a file; Enter hands it
  to the library. Folders, PDFs and a plain launch
  still open the library, and so does `--library` with any file.
- The viewer's window can float centred like imv and mpv; the rule ships in
  `/usr/share/omaroll/hypr/omaroll.lua`.
- Select text in a PDF. Drag across a page to pick the words under the pointer,
  see each selected line highlighted, then copy those words with Copy
  selection or Ctrl+C. The words come from the same Poppler tool the document
  search uses, so nothing new is installed and the file is never changed.
- The demo library now carries a two-page document, so the page list, the find
  row and the page controls are rendered and reviewed like every other view.

### Changed

- Opening a picture or video from outside no longer starts the library. The
  library's services are built the first time it is asked for, in the same
  process, so marks set in either window show in both.
- Secondary text now takes its colour from the theme's own muted colour, which
  the theme backend already picks for at least 3:1 contrast, rather than from a
  fixed alpha. The status line, section labels, placeholders, empty rating stars
  and the action shortcuts were drawn at 0.28 to 0.42 alpha, which measured
  about 1.7:1 on a light theme and read as blank.
- PDF pages are drawn with a hairline edge. White paper on a light theme had
  nothing to separate it from the stage behind it.
- On a narrow window the document rows keep the page, its navigation, the fit
  choice and text selection: the page turns become arrows, the search box
  narrows, and the pills that need more room or repeat the action list (match
  stepping, Copy page text, the page number box, Copy selection) wait until the
  window is wider. Ctrl+C still copies a selection.

### Fixed

- A video with one audio track and no subtitles showed "Audio 1" and "CC"
  drawn over the clock in the preview's controls. Those pills now hide when
  there is nothing to switch, rather than only shrinking to no width.
- The mouse wheel did not zoom a picture in the preview when Qt on Wayland
  reported the wheel as a touchpad. Wheel notches now zoom whatever the device
  is called, and a two-finger touchpad swipe pans a zoomed picture.
- The continuous PDF view collapsed each page to a sliver as soon as the page
  rendered, and a fitted page was stretched to the aspect of the stage box it
  was requested in. Both read the size asked of the renderer instead of the
  page's own size.
- The document control rows overlapped each other and ran under the details
  panel on a narrow window; they now measure their labels in the theme font and
  shrink before they collide.

## 1.8.0

### Added

- Lossless rotate and flip. A JPEG rotated or flipped with no crop or resize is
  written without recompressing the pixels (through jpegtran where available),
  so a turn no longer costs quality; every other correction recompresses as
  before. An image with an EXIF orientation tag still takes the recompressing
  path, which bakes the tag in.
- Crop aspect presets. The correction editor holds the crop to Free, Original,
  1:1, 4:3, 3:2 or 16:9 while the frame is dragged, snapping to the largest
  centred rectangle of that ratio when the preset is chosen.
- Straighten. A fine ±15° level control in the correction editor, scaled to
  fill the frame so no empty corners show, applied before the crop.
- Correct a selection. Rotate, flip or resize several selected pictures at
  once (B), each written as its own copy with the existing collision handling.
- PDF continuous scroll. Pages scroll at the window width by default, with a
  Fit page mode for reading a whole page with zoom and pan.
- Print. A Print action for pictures and PDFs hands the file to the system
  print path (CUPS), and is greyed out with an install hint when it is absent.
- Undo organization marks. Ctrl+Z steps back through favourites, hidden flags,
  ratings and captions, one action at a time.
- Subtitle timing. Sidecar subtitles can be nudged earlier or later by half a
  second to fix a track that is out of sync.
- Slideshow options. The slideshow interval is configurable (2 to 20 seconds)
  and a shuffle mode picks a random picture instead of the next in order.

## 1.7.0

### Added

- Crop, rotate, flip and resize a picture as a copy. The viewer's action list
  gains Crop, rotate, resize (Q): a live preview with a draggable crop frame,
  quarter turns and flips, and an optional output size. The original is never
  modified; the copy is written beside it as `<name>-edited.<ext>`, numbered on
  repeat. EXIF orientation is baked in, the ICC profile is kept, and collision
  or write failures leave the original untouched.
- Rename an album or a tag without losing its membership. Renaming a tag also
  renames every tag nested under it (Travel/Japan follows Travel). A name that
  clashes with an existing collection is refused.
- Compare pictures side by side with synchronized zoom and pan (K). It uses the
  checked selection, or the open picture's exact duplicates and then its
  visually similar set, and shows each file's name.
- Remember video playback. Volume and mute are kept across files and sessions,
  and a video reopened part-way through offers Resume or Start over instead of
  always starting from the beginning. Watching to the end clears the spot.
- External subtitles. A `.srt` or `.vtt` beside a video is offered alongside any
  embedded tracks, labelled by its language tag (movie.en.srt shows as
  English), and drawn over the video as playback reaches each cue.
- Find text in a PDF. The viewer's PDF controls gain a search box; each match
  is a page you can step through, with a count, and the page turns as you move
  between matches.
- Copy a PDF page's text to the clipboard, and jump straight to a page by
  typing its number in the PDF controls.

### Changed

- Favourites, hidden flags, ratings and captions now follow a file that was
  moved or renamed outside Omaroll, using the same inode or content-fingerprint
  recovery that albums and tags already had. A file that is genuinely gone
  still has its marks dropped, and an equal-sized unrelated file is not
  mistaken for it.
- Back up and restore the organization as a versioned JSON file: albums, tags,
  favourites, hidden files, ratings, captions and saved views. Export writes
  atomically; restore validates the whole file before changing anything, so a
  bad or truncated file leaves the current profile intact. Media files are
  never touched.
- Copy a selected region. In the crop editor, Copy region puts just the cropped
  area (after any rotation or flip) on the clipboard, without writing a file.

## 1.6.0

### Added

- Star ratings. Rate a file from the viewer or with Alt+1 to Alt+5, sort by
  Top rated, and narrow any view to a minimum rating from Browse. Saved views
  keep the choice.
- Captions. Add a short caption in the viewer; it shows on the tile and is
  searched with filenames and picture text.
- Nested tags. A tag named Travel/Japan sits under Travel in Browse, and
  choosing the parent shows everything beneath it.
- Browse a photo library by camera and lens. The metadata pass that reads
  capture dates now records the camera and lens too, and saved views keep
  the choice. Libraries without camera photos do not show the section.
- Tab and Shift+Tab step through the sections, wrapping at either end.

### Changed

- A video that plays to its end rewinds to its first frame and pauses there
  instead of leaving a blank stage.

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
