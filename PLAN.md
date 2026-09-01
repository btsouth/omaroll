# omaroll

**Everything you capture, in one place.**

A local-first capture library for Omarchy. It finds every screenshot, screen recording, picture and
download you already have, arranges them by day, and hands each one to the tool that already owns
the job. It replaces nothing. It just gives all of it a home.

- Stack: C++20 · Qt 6.8 Quick · CMake + Ninja
- License: GPL-3.0-or-later
- Reuses the omakade skeleton
- Offline by default, no telemetry, no accounts

Status: planning. Nothing public yet.

---

## 1. The gap

Omarchy ships a complete capture stack and then abandons the output in a folder.

| Step | What ships with Omarchy | State |
|---|---|---|
| Screenshot | `omarchy-capture-screenshot` · grim/slurp | Solid |
| Screen recording | `omarchy-capture-screenrecording` · gpu-screen-recorder | Solid |
| Video trim | `omacut` 0.4.0 · Qt Quick + ffmpeg | Solid |
| Image edit | `pinta` | Covered |
| Text from image | `omarchy-capture-text` · tesseract | Covered |
| Viewing | `imv` · `sushi` · `evince` · `mpv` | Covered |
| **Find what you captured** | nautilus, on a flat folder | **Missing** |
| **Do the next thing with it** | you, by hand, every time | **Missing** |

What is missing is not another editor. It is the two steps after capture.

What that looks like on a real machine:

```
$ ls ~/Videos
screenrecording-2026-08-30_20-10-45.mp4
screenrecording-2026-08-30_20-10-59.mp4
screenrecording-2026-08-30_20-11-49.mp4
screenrecording-2026-08-30_20-12-04.mp4
screenrecording-2026-08-31_23-26-39.mp4
...

# 188 capture files across ~/Pictures and ~/Videos.
# Timestamp-named, no thumbnails, indistinguishable.
```

The chain today:

```
  capture  ->  lands in    ->  [ find it ]  ->  [ act on it ]      tools that could
  grim         ~/Pictures      nothing          nothing            omacut · pinta
  gsr          ~/Videos                                            mpv · tesseract
                               ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                               the two boxes that never connect
```

omaroll is the wire between them.

---

## 2. The thesis

Omakade did not win because it was a nice grid. It won because it understood what the files were
and handed the action to whoever already owned it.

It does not launch games. It asks Steam to. That is why it read as native instead of as a competing
launcher, and why it needed so little code to feel complete. omaroll makes the identical move on
captures: it knows a `.mp4` in `~/Videos` named `screenrecording-*` is a screen recording, and it
knows the next thing you want to do with one is trim it, so it hands it to omacut.

**Every handler in the action matrix already ships with Omarchy, with exactly one exception.**
omaroll adds a front door and one new capability, and otherwise makes the tools you already have
reachable. That is the same claim that made omakade credible.

It also asks nothing of the user. A capture-tool rewrite would ask people to rebind PrtScn and learn
a new flow, competing with a keybinding that already works. omaroll asks for nothing: install it,
and 188 files you already own are suddenly arranged and actionable. That difference is most of why
the first one spread.

---

## 3. Principles

Carried over from omakade because they are proven, not because they are convenient.

- **Read-only on discovery.** Never move, rename, or rewrite a file it finds. Every action that
  produces output writes a new file beside the original.
- **Delegate the verb.** If a tool on the system already owns an action, launch it rather than
  reimplement it. The bar for building something natively is that nothing on Omarchy does it at all.
- **Zero config.** First run shows your existing captures with no import, no setup, no account.
  Settings change defaults, they never make the app work.
- **Theme-native and live.** Reads the active Omarchy palette, font and transparency, and follows
  theme changes without a restart.
- **Offline.** Nothing in the core path touches the network. No telemetry, no sync, no accounts.
- **Fast on a real library.** Cold start under a second on several thousand files. Thumbnails cached
  and generated off the main thread.
- **Keyboard first.** Every action reachable without the mouse, in the Omarchy idiom.

---

## 4. Scope

The out-list is the more important half. Guard it.

### In

- Unified library across screenshots, recordings, pictures, videos, downloads
- Day grouping, kind sections, search, favorites, hide
- Thumbnails for images and video, hover-scrub on video
- Multi-select and bulk actions
- Delegated actions to installed tools
- Mattes: the one native capability
- Copy, drag out, rename, delete with confirm
- Live theme following
- `--demo` mode with a fictional library

### Out

- **Video editing.** omacut owns it and is good. Never rebuild it.
- **Image editing.** pinta owns it.
- **Capture itself.** `omarchy-capture-*` owns it. No hotkey grab, no overlay, no PrtScn fight.
- **Scrolling capture.** Needs synthetic input into another window. Wayland blocks it by design.
  Not possible, do not try.
- A file tree, or anything that reads as a file manager
- Cloud sync, accounts, sharing service
- Tag taxonomies and albums at v1
- RAW photo workflows

> **The rule that keeps it small:** if a feature request can be answered with the name of a package
> already in `omarchy-base.packages`, the answer is a row in the action matrix, not a module in the
> codebase.

---

## 5. Action matrix

The core of the app. Each capture kind has a default action and a short menu behind it.

| Capture kind | Action | Handler | Who |
|---|---|---|---|
| Screen recording | Trim *(default)* | `omacut` | base pkg |
| Screen recording | Clip to GIF | ffmpeg one-shot | via omacut dep |
| Screen recording | Resize / re-encode | ffmpeg one-shot | via omacut dep |
| Screen recording | Play | `mpv` | base pkg |
| Screenshot | Make it postable *(default)* | matte composer | **native** |
| Screenshot | Copy to clipboard | `wl-copy` | base pkg |
| Screenshot | Copy the text | `tesseract` | base pkg |
| Screenshot | Real edit | `pinta` | base pkg |
| Any image | Scan a QR code | `zbarimg` | base pkg |
| Any image | View full size | `imv` | base pkg |
| Any | Send to phone | `localsend` | base pkg |
| Any | Send to another machine | `omarchy-tailscale-send` | omarchy bin |
| Any | Show in files | `nautilus` | base pkg |
| Any | Delete | trash, with confirm | **native** |

One native row out of fourteen. That ratio is the design, and it is what keeps the codebase small
enough to finish.

The action registry is **data, not code**: adding a handler is a table entry with a match predicate
and an argv template, so a contributor can wire up a new tool without touching C++.

---

## 6. Architecture

Qt 6.8 Quick with a C++20 core, CMake and Ninja, packaged as a native Arch package. This is the
Omarchy first-party house stack (omacut, omawrite, omacalc all use it), it is what omakade already
builds with, and it means the theme layer, settings, single-instance handling, packaging and CI all
transfer rather than getting rewritten.

### What lifts from omakade

| From omakade | Into omaroll | Work |
|---|---|---|
| `src/theme/OmarchyTheme.{h,cpp}` | Same file, unchanged | Lift |
| `src/app/AppSettings.{h,cpp}` | New keys, same shape | Lift |
| `src/app/SingleInstance.{h,cpp}` | Same file, unchanged | Lift |
| `src/app/main.cpp` | Same bootstrap, new roots | Lift |
| `qml/components/GlassButton.qml` | Same file, unchanged | Lift |
| `LibraryFilterModel` | `CaptureFilterModel` | Adapt |
| `UnifiedGameModel` | `UnifiedCaptureModel` | Adapt |
| `qml/components/GameCard.qml` | `CaptureCard.qml` | Adapt |
| `qml/components/LibraryView.qml` | `CaptureGrid.qml` | Adapt |
| `packaging/arch/PKGBUILD.in` | New deps, same template | Adapt |
| `.desktop` + `metainfo.xml` | Same shape | Adapt |
| CI, release, SHA256SUMS flow | Same workflow | Lift |

omakade is roughly 14,000 lines across `src/` and `qml/`, and a real slice of that is scaffolding
rather than game logic. Starting from that skeleton instead of an empty CMakeLists is worth several
weeks on its own, and it guarantees the two apps look and feel like siblings.

### Source layout

```
src/
  app/        main.cpp  AppSettings  SingleInstance     lift
  theme/      OmarchyTheme                              lift
  library/    CaptureRoles.h
              CaptureRecord
              UnifiedCaptureModel                       adapt
              CaptureFilterModel                        adapt
              MockCaptureModel      --demo
  sources/    ScreenshotSource   ~/Pictures, name-matched
              RecordingSource    ~/Videos,   name-matched
              PictureSource      ~/Pictures, the rest
              VideoSource        ~/Videos,   the rest
              DownloadSource     ~/Downloads, media only
  thumbs/     ThumbnailCache
              ImageThumbnailer   libvips
              VideoThumbnailer   ffmpegthumbnailer
              ScrubStrip         lazy frame extraction
  actions/    ActionRegistry     the matrix, as data
              ProcessLauncher    argv templates, no shell
  matte/      MatteComposer      native
              HueExtractor       native
  text/       OcrService         tesseract

qml/
  Main.qml
  components/ CaptureCard  CaptureGrid  DayHeader
              ScrubThumb   ActionBar    SectionTabs
              MattePicker  GlassButton
  screens/    CaptureDetail  MatteSheet  Settings
```

---

## 7. Discovery and classification

The trick that makes sections possible: Omarchy names its own captures predictably.

Screenshots land in `~/Pictures` alongside every other image, and recordings land in `~/Videos`
alongside every other video. A pure extension check cannot separate "a screenshot I took" from "a
photo I downloaded." But `omarchy-capture-screenshot` writes
`screenshot-YYYY-MM-DD_HH-MM-SS.png` and `omarchy-capture-screenrecording` writes
`screenrecording-YYYY-MM-DD_HH-MM-SS.mp4`. That naming is the classifier.

### Classification order

1. **Filename pattern first.** Omarchy's own capture prefixes, plus common ones from grim,
   flameshot, gpu-screen-recorder, OBS, and yt-dlp output templates.
2. **Then MIME and extension** for everything else, into Pictures or Videos.
3. **Never by directory alone.** A user who redirects captures elsewhere still gets them classified
   correctly.

### Watched locations

Resolved from the environment so a customized setup works without configuration, each overridable
in settings:

```
$OMARCHY_SCREENSHOT_DIR    fallback $XDG_PICTURES_DIR  fallback ~/Pictures
$OMARCHY_SCREENRECORD_DIR  fallback $XDG_VIDEOS_DIR    fallback ~/Videos
$XDG_DOWNLOAD_DIR          fallback ~/Downloads
```

Live updates via `QFileSystemWatcher` on the watched roots, so a capture taken while omaroll is open
appears at the top without a rescan.

Index and metadata cache in `~/.local/share/omaroll/`, thumbnails in `~/.cache/omaroll/thumbs/`.
Both safe to delete at any time.

### The record

```
CaptureRecord {
  path        QString
  kind        Screenshot | Recording | Picture | Video | Download
  captured    QDateTime   parsed from name, else mtime
  bytes       qint64
  size        QSize       images and video
  duration    int         video, ms
  thumb       QString     cache path, lazily filled
  favorite    bool
  hidden      bool
}
```

---

## 8. Mattes

The one thing omaroll builds natively, because nothing on Linux does it.

Click a screenshot, get six finished backgrounds derived from the image's own dominant hue, pick
one, and the composed PNG is on your clipboard and saved beside the original. No editor in the flow.
Choosing between finished results replaces tweaking one.

This is the single idea worth taking from matteshot, and it is the one part of that codebase that
actually moves. `compose.rs` is about 850 lines of pure image math on the `image` crate with no
Windows API in it at all, so the algorithm ports cleanly to a Qt/libvips implementation. The other
45,000 lines are hand-drawn Win32, GDI and Media Foundation, or licensing and telemetry scaffolding
for a paid Windows tray app. None of that belongs here.

**Why it earns its native slot:** it is the only row in the action matrix with no existing handler,
it is what makes screenshots of omaroll itself look expensive, and it answers "how do I make this
postable" which is the actual reason people ask for image tools they do not otherwise want.

### The seven

- **Adaptive** and **Deep**, light and dark gradients from the extracted hue
- **Aurora**, soft mesh blobs
- **Slate** and **Paper**, neutral dark and neutral light
- **Pop**, the complementary hue
- **None**, the raw capture, so the picker is never a tax

Deterministic per capture, so the same screenshot always offers the same six. Padding and a few
aspect presets (Original, 1:1, 16:9, social 1.91:1) sit alongside. Non-destructive always: the
original is untouched and the composite is a new file.

---

## 9. Thumbnails and performance

Both thumbnailers are already in `omarchy-base.packages`, so this costs no new dependencies.

- **Images** through `libvips`, in base, fastest option available.
- **Video** through `ffmpegthumbnailer`, also in base.
- **Hover-scrub** extracts a small strip of frames lazily on first hover and caches it. Never on
  scan, never for the whole library.
- **Cache** keyed on path + mtime + size, bounded, in `~/.cache/omaroll/thumbs/`.
- **Generation off the main thread** via `QtConcurrent`, bounded queue prioritized by what is
  currently on screen.

### Budget

| Measure | Target |
|---|---|
| Cold start to first paint, 5,000 files | < 1s |
| Warm start, cache populated | < 400ms |
| Scroll | 60fps, no dropped frames on a full grid |
| Hover-scrub first frame | < 150ms |
| Idle memory, 5,000 files | < 250MB |

The index is metadata only. Full-size images are never held in memory, and the grid is a recycling
view, so a 20,000 file library costs the same as a 2,000 file one to scroll.

---

## 10. Interface

A library, not a file manager. The distinction is enforced by what is absent.

One window. A row of section tabs across the top, a search field, and a grid of thumbnails grouped
by day with a sticky date header. No sidebar tree, no path bar, no columns view. If it starts to
look like nautilus, something has gone wrong.

### Sections

- **Recent**, everything, newest first. The default view.
- **Screenshots** and **Recordings**, the two hero sections
- **Pictures** and **Videos**, everything else in those folders
- **Downloads**, media only
- **Favorites**

### Keyboard

| Key | Does |
|---|---|
| arrows / hjkl | Move through the grid |
| `/` | Search |
| Enter | Default action for that kind |
| Space | Large preview, play video in place |
| `m` | Matte picker (screenshots and images) |
| `t` | Trim in omacut (recordings) |
| `c` | Copy to clipboard |
| `s` | Send, localsend or tailscale |
| `f` | Favorite |
| `1`-`6` | Jump to section |
| Del | Delete, with confirm |
| Esc | Back, then close |

Drag a thumbnail out and it drops as the real file into any Wayland target, which covers "put this
in a Discord message" without omaroll knowing anything about Discord.

---

## 11. Milestones

Each one is independently shippable. Order matters, so it is numbered.

### v0.1 — It sees your captures

Scaffold from the omakade skeleton. Theme, settings, single-instance and packaging all transfer on
day one.

- CMake project, Qt Quick shell, OmarchyTheme wired and following theme changes
- Screenshot and Recording sources with filename classification
- Static grid, day grouping, image thumbnails via libvips
- Open in default handler, show in files

*Internal. The moment it is worth a screenshot.*

### v0.2 — It is fast and complete

- Video thumbnails via ffmpegthumbnailer, hover-scrub strips
- Remaining sources: Pictures, Videos, Downloads
- Thumbnail cache, background generation, on-screen priority
- Search, favorites, hide, sort
- `QFileSystemWatcher` live updates
- Full keyboard navigation

*Internal. Hits the performance budget here or it gets fixed here.*

### v0.3 — It does things

The action registry lands, and with it the whole delegation argument.

- ActionRegistry as data, ProcessLauncher with argv templates and no shell
- Trim to omacut, play in mpv, edit in pinta, view in imv
- ffmpeg one-shots: clip to GIF, resize, extract frame
- OCR to clipboard via tesseract, QR via zbarimg
- Send via localsend and omarchy-tailscale-send
- Multi-select and bulk actions
- Actionable errors when a handler is missing, naming the package to install

*First public beta. Post it to the Omarchy Discord.*

### v0.4 — Mattes

- HueExtractor and MatteComposer, all seven mattes, ported from compose.rs
- MattePicker sheet, six variants, keyboard select
- Padding and aspect presets, output size caps
- Composite to clipboard and to a new file beside the original

*The feature the launch video opens on.*

### v0.9 — Release candidate

- `--demo` mode with a deterministic fictional library, same as omakade, so launch material never
  exposes real files
- Settings screen: watched folders, default actions per kind, cache size
- Empty states, first-run state, permission and missing-folder handling
- Test pass, including classification against real-world filename patterns
- README, screenshots, demo video, project page

*Tagged RC, tested on a clean Omarchy VM.*

### v1.0 — Ship

- GitHub release with `.pkg.tar.zst` and SHA256SUMS
- PKGBUILD submitted to OPR
- Launch post

---

## 12. Packaging and distribution

The exact path omakade already took, which means it is known to work.

```bash
pkgname=omaroll
pkgdesc='Everything you capture, in one place'
license=('GPL-3.0-or-later')
depends=('qt6-base' 'qt6-declarative' 'qt6-wayland'
         'libvips' 'ffmpegthumbnailer')
optdepends=('omacut: trim recordings'
            'tesseract: copy text out of a screenshot'
            'zbar: scan QR codes'
            'pinta: edit images'
            'mpv: play video'
            'imv: view images full size'
            'localsend: send to another device'
            'wl-clipboard: copy to clipboard')
makedepends=('cmake' 'ninja' 'pkgconf')
```

Only two hard dependencies beyond Qt, and both are already in `omarchy-base.packages`. Every
optdepend is also already installed on a stock Omarchy box, so the optdepends list functions as
documentation rather than as work for the user. On a non-Omarchy Arch system it degrades gracefully:
a missing handler greys its action out and names the package.

### Channels

- **OPR** as the headline: `sudo pacman -S omarchy/omaroll`, then it updates with the system.
- **GitHub release** with the prebuilt package and SHA256SUMS, verified install in three commands.
- **AUR** for Arch users outside Omarchy.
- Ship `.desktop` and `metainfo.xml` so it appears in the launcher properly.

---

## 13. Launch

This one has a hook omakade did not have. Use it.

**The loudest complaint about Omarchy is video editing. omaroll is not a video editor, and the post
should say so in the first line.**

Almost nobody asking for video editing wants kdenlive. They want to cut four seconds off a clip and
post it. Between omacut for the trim, ffmpeg one-shots for GIF and resize, and a library that puts
both one keystroke from the recording, that request is answered without anyone building an NLE.
Saying "I didn't build a video editor, I connected the ones you already have" is a stronger post
than any feature list, and it is true.

### Materials

- An 18 second demo, same length and register as the omakade one: the grid filling with real
  thumbnails, then one screenshot to matte to clipboard, then one recording straight into omacut
- Shot in `--demo` mode so nothing personal is on screen
- Project page matching omakade's, so the two read as a family
- Post to the Omarchy Discord at v0.3, not at v1.0. That audience finds the classification edge
  cases you will never hit alone.

---

## 14. Risks

| Risk | Mitigation |
|---|---|
| It reads as a worse file manager | No tree, no path bar, no columns. Day grouping, kind sections and a default action per kind are what make it a library. Main thing to protect in review. |
| Pictures and Videos sections overlap nautilus | They are secondary tabs. Screenshots and Recordings are the hero, and they are the two nautilus genuinely cannot help with. |
| Thumbnail generation on a huge library | Bounded queue, on-screen priority, persistent cache. Budget set at v0.2 and enforced there. |
| Classification misses non-Omarchy filenames | Pattern list covers grim, flameshot, OBS, gpu-screen-recorder and yt-dlp templates, falls back to extension, user-extendable. Beta at v0.3 exists to collect the misses. |
| Scope creep into editing | The out-list, and the rule that a request answerable with a package name becomes a matrix row. |
| Upstream ships something similar | Move fast and stay compatible rather than competitive. The matte is the part nothing else has, and delegating to omacut means an upstream improvement to omacut makes omaroll better rather than redundant. |

---

## Appendix: why not port matteshot

Measured before deciding. matteshot is 46,429 lines of Rust across 29 modules, and the Windows
coupling is not a thin layer.

| Module | Lines | Portable? |
|---|---|---|
| `tweak.rs` | 5,946 | No, hand-drawn Win32/GDI editor |
| `recdone.rs` | 5,743 | No, and duplicates omacut |
| `history.rs` | 2,725 | Concept yes, code no, GDI-drawn |
| `main.rs` | 2,558 | No, Win32 message loop |
| `settings.rs` | 2,540 | No |
| `license.rs` | 2,407 | Should not come, commercial scaffolding |
| `trim.rs` | 1,725 | No, and duplicates omacut |
| `record.rs` | 1,697 | No, Media Foundation |
| `overlay.rs` | 1,580 | No, Win32 |
| `scroll.rs` | 1,436 | **Impossible on Wayland** |
| `compose.rs` | 857 | **Yes.** Pure image math, no Windows API |

Three specific findings:

1. **Scroll capture cannot come.** The algorithm is good, but it depends on synthesizing wheel input
   into another app's window. Wayland blocks that by design. Hyprland gives virtual input but not
   "scroll that window over there."
2. **About 9,000 lines should not come anyway.** `license.rs`, `license_ui.rs`, `share.rs`,
   `release_manifest.rs`, `installer.rs`, `update.rs`, `telemetry.rs`, `welcome.rs` are commercial
   scaffolding for a paid Windows tray app.
3. **The recording editor duplicates omacut.** `recdone.rs` + `trim.rs` are 7,400 lines of video
   editor, and Omarchy already has the trimmer.

So "port matteshot" is a rewrite that keeps the ideas. The ideas are the valuable part, and exactly
one of them (the matte) belongs in omaroll.
