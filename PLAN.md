# omaroll

**Everything you capture, in one place.**

A local-first capture library for Omarchy. It finds every screenshot, screen recording, picture and
download you already have, arranges them by day, and hands each one to the tool that already owns
the job. It replaces nothing. It just gives all of it a home.

- Stack: C++20 · Qt 6.8 Quick · CMake + Ninja
- License: **MIT** (see section 14; this is an inclusion requirement, not a preference)
- Reuses the omakade skeleton
- Offline by default, no telemetry, no accounts

Status: **v1.0.0 release candidate built and tested. GitHub publication authorized.**

---

## 1. The gap

Omarchy ships a complete capture stack and then abandons the output in a folder.

| Step | What ships with Omarchy | State |
|---|---|---|
| Screenshot | `omarchy-capture-screenshot` · grim/slurp | Solid |
| Screen recording | `omarchy-capture-screenrecording` · gpu-screen-recorder | Solid |
| Video trim | `omacut` 0.4.0 · Qt Quick + ffmpeg | Solid |
| Annotate a screenshot | `tensaku` · wired as `$OMARCHY_SCREENSHOT_EDITOR` | Covered |
| Image edit | `pinta` | Covered |
| Convert / resize | `omarchy-transcode` · jpg/png/mp4/gif | Covered |
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

**Every handler in the action matrix already ships with Omarchy.**
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
- **Theme-native and live.** Reads the active Omarchy palette, font, corner radius and surface alpha,
  and cross-fades to a new theme without a restart or a flash. Section 11.
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
- Named albums that follow renamed and moved files without modifying them
- Thumbnails for images and video, hover-scrub on video
- Multi-select and bulk actions
- Delegated actions to installed tools
- Mattes: the one native capability
- Copy, drag out, rename, delete with confirm (XDG trash, never `unlink`)
- Live theme following
- `--demo` mode with a fictional library

### Out

- **Video editing.** omacut owns it and is good. Never rebuild it.
- **Annotation.** tensaku ships in base and is already wired as the screenshot editor. Never rebuild it.
- **Image editing.** pinta owns it.
- **Format conversion.** `omarchy-transcode` owns it. omaroll writes no ffmpeg invocations.
- **Capture itself.** `omarchy-capture-*` owns it. No hotkey grab, no overlay, no PrtScn fight.
- **The moment right after a capture.** `omarchy-capture-screenshot` already fires a notification
  reading "Screenshot saved to clipboard and file / Edit with Super + Alt + ," whose click action
  runs `$SCREENSHOT_EDITOR`. That path exists and works. omaroll is the *later* path, for the
  screenshot you took an hour ago. Do not add a competing toast.
- **Scrolling capture.** Needs synthetic input into another window. Wayland blocks it by design.
  Not possible, do not try.
- A file tree, or anything that reads as a file manager
- Cloud sync, accounts, sharing service
- Tag taxonomies
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
| Screen recording | Clip to GIF | `omarchy-transcode <p> gif 720p` | omarchy bin |
| Screen recording | Resize / re-encode | `omarchy-transcode <p> mp4 1080p` | omarchy bin |
| Screen recording | Play | `mpv` | base pkg |
| Screenshot | Make it postable *(default)* | matte composer | **native** |
| Screenshot | Annotate | `$OMARCHY_SCREENSHOT_EDITOR`, default `tensaku-edit` | base pkg |
| Screenshot | Copy the text | `tesseract` | base pkg |
| Any image | Copy to clipboard | `omarchy-clipboard-paste-file --copy-only` | omarchy bin |
| Any image | Convert / resize | `omarchy-transcode <p> jpg medium` | omarchy bin |
| Any image | Real edit | `pinta` | base pkg |
| Any image | Scan a QR code | `zbarimg` | base pkg |
| Any image | View full size | `imv` | base pkg |
| Any | Send to phone | `omarchy-menu-share file` → `localsend --headless send` | omarchy bin |
| Any | Send to another machine | `omarchy-tailscale-send <machine>`, needs a peer picker first | deferred |
| Any | Show in files | `nautilus` | base pkg |
| Any | Delete | `QFile::moveToTrash()`, with confirm | **native** |

Specialized media work stays delegated. Omaroll owns only the library behavior, safe trash and the
matte composer, which is what keeps the codebase small enough to finish.

Rows are matched on medium, still or moving, not on the section a file sits in. A downloaded clip
trims like a recording and a downloaded photo mattes like a screenshot. Tailscale is deferred:
`omarchy-tailscale-send` takes the machine as its first argument and there is no house picker for
one outside the shell panel, so shipping the row would have sent the file path as the machine name.

Three of these were corrected after auditing the Omarchy tree, and each removed code rather than
adding it:

- **Annotation is not a gap.** `tensaku` ships in base and is already the designated screenshot
  editor: `omarchy-capture-screenshot` sets `SCREENSHOT_EDITOR="${OMARCHY_SCREENSHOT_EDITOR:-tensaku-edit}"`,
  `omarchy-clipboard-open` opens images in it, and imv binds `Ctrl+E` to it. omaroll must honor the
  same env var so a user who overrode it gets their editor here too.
- **No ffmpeg code at all.** `omarchy-transcode` already does pictures to jpg/png at high/medium/low
  and videos to mp4/gif at 4k/1080p/720p, non-interactively when given arguments. Shelling to it
  beats hand-rolling ffmpeg invocations and keeps the behavior consistent with the menu's own
  Transcode entry.
- **Clipboard goes through the house helper.** `omarchy-clipboard-paste-file --copy-only image/png <path>`
  rather than calling `wl-copy` directly.

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

### Theming: the mechanism is already correct, do not "fix" it

`OmarchyTheme` watches `~/.local/state/omarchy/current/theme/colors.toml` and `shell.toml` with a
`QFileSystemWatcher`. That is exactly the contract in Omarchy's `docs/theming.md`: `omarchy-theme-set`
materializes the active theme into `~/.local/state/omarchy/current/theme` and then notifies.

This means omaroll deliberately does **not** need an entry in `post_theme_commands` in
`bin/omarchy-theme-set`. That list exists for apps that cannot notice a theme change on their own
and need an external retint kick (terminals, btop, VS Code, Obsidian). An app that watches the state
directory retints itself, with no upstream change required and no restart. Self-watching is both the
better behavior and one fewer patch to land upstream. Leave it alone.

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

### Traversal rules

Left unstated in the first draft, and every one of these is a real-library problem:

- **Screenshots and Recordings scan depth 1.** Omarchy writes captures flat into the configured
  directory, so recursing only invites unrelated files.
- **Pictures and Videos recurse**, to a bounded depth of 4. People foreseeably keep photos in
  subfolders, and a library that cannot see them looks broken.
- **Skip dotfiles and dot-directories.** `~/Pictures/.thumbnails` and similar caches must never
  appear as content.
- **Skip omaroll's own output directories** so a matte composite does not read as a new capture that
  then gets its own matte.
- **Follow symlinks** (`find -L` semantics, matching what Omarchy's own image picker does) but track
  visited inodes so a symlink loop cannot hang the scan.
- **Deduplicate by realpath**, since a symlinked directory can otherwise present the same file twice.

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
libvips in particular was added to base by migration `1786386460` specifically to generate image
picker thumbnails, so it is the house thumbnailer and using it is the consistent choice, not just a
fast one.

- **Images** through `libvips`, in base, fastest option available.
- **Video** through `ffmpegthumbnailer`, also in base.
- **Hover-scrub** extracts a small strip of frames lazily on first hover and caches it. Never on
  scan, never for the whole library.
- **Cache** keyed on path + mtime + size, bounded, in `~/.cache/omaroll/thumbs/`. Match the key
  Omarchy's own image picker already uses in `shell/plugins/image-picker/list.sh`:
  `stat -Lc '%s:%Y'` hashed with the path, thumbnails written as `<hash>.jpg`, index in a sidecar.
  Same shape, different directory.
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

## 11. Look and feel

Transparency, and following the theme so smoothly that a theme change looks intentional. Both are
requirements, and Omarchy has specific machinery for each. Getting this wrong is the difference
between an app that looks like it shipped with the OS and one that looks bolted on.

### Omarchy's actual visual grammar

Checked rather than assumed, in `default/hypr/looknfeel.lua`:

```lua
decoration = {
  rounding = 0,
  shadow = { enabled = false },
  blur   = { enabled = false },
}
```

**Blur is off. Rounding is zero. Shadows are off.** Omarchy is deliberately flat and sharp, closer
to a terminal than to macOS glass. Window translucency is a whisper, not an effect:
`default/hypr/windows.lua` tags every window `default-opacity` and applies `opacity = "0.985 0.96"`,
so 98.5% focused and 96% unfocused.

Any design that assumes frosted glass will look wrong here, and will look wrong to upstream. Build
flat, sharp and quiet, and let translucency be a subtlety rather than the point.

### The rule that decides omaroll's transparency

`default/hypr/apps/system.lua`:

```lua
-- No transparency on media windows.
o.window("^(zoom|vlc|mpv|org.kde.kdenlive|com.obsproject.Studio|com.github.PintaProject.Pinta|imv|org.gnome.NautilusPreviewer)$",
  { tag = "-default-opacity" })
... { opacity = "1 1" }
```

**Every media app in Omarchy is explicitly opted out of window transparency.** mpv, vlc, imv,
kdenlive, OBS, Pinta, the Nautilus previewer. The reason is obvious once stated: dimming the pixels
someone is trying to look at is a bug, not a style.

omaroll is a media app. A grid of thumbnails rendered at 96% opacity has wrong colors, and for a
photo and video library that is a correctness problem, not a taste problem.

So omaroll does both halves deliberately:

| Layer | Treatment | Why |
|---|---|---|
| Hyprland window opacity | **Opt out.** `tag = "-default-opacity"`, `opacity = "1 1"` | Compositor dimming hits thumbnails too, and would stack on top of the app's own alpha |
| App chrome: background, headers, action bar, sheets | **In-app alpha from the theme** | Translucency where it costs nothing |
| Thumbnails, previews, video frames | **Fully opaque, always** | The content is the point |
| Text | **Fully opaque, always** | Legibility |

That combination is what "transparent and beautiful" actually means on this desktop: the surface
recedes, the pictures do not.

### The mechanism, exactly as omakade does it

Verified in omakade's source rather than described from memory. Three pieces:

```qml
// qml/Main.qml
ApplicationWindow {
  color: "transparent"              // the window surface itself carries no paint
  ...
  Rectangle {                        // the chrome, painted at theme alpha
    gradient: Gradient {
      GradientStop { position: 0.0;  color: root.alpha(Theme.darkerBackground, Theme.surfaceAlpha) }
      GradientStop { position: 0.48; color: root.alpha(Theme.darkerBackground, Theme.surfaceAlpha * 0.88) }
      GradientStop { position: 1.0;  color: root.alpha(Theme.darkerBackground, Theme.surfaceAlpha) }
    }
  }
}
```

```cpp
// src/theme/OmarchyTheme.cpp: surfaceAlpha, default 0.82
m_surfaceAlpha = readSectionAlpha(themeRoot() + "/shell.toml", "launcher", "background-alpha", 0.82);
```

So: transparent window, one translucent gradient for the chrome at roughly 82% by default, and
everything drawn on top of it is opaque. Content sits above the translucency rather than inside it.

**That is exactly what omaroll wants, and it is the answer to "the pictures don't need to be
transparent but the launcher should."** Same window, same alpha source, same default. The grid,
thumbnails, previews and text are children painted at full opacity on top of the translucent
surface, so the chrome glows and the photographs stay true.

Lift `readSectionAlpha` and the `alpha(color, value)` helper unchanged.

### One thing omakade does not do, and omaroll should

omakade is a third-party app, so it is not in `default/hypr/apps/system.lua` and never opted out of
the `default-opacity` tag. Its cover art is therefore composited by Hyprland at `0.985` focused and
`0.96` unfocused, on top of its own in-app alpha.

At 98.5% that is imperceptible, and for game covers it does not matter. For a library whose entire
job is letting someone compare photographs and video frames, the unfocused `0.96` is worth removing,
and it is one line upstream. This is not a defect in omakade; it is a difference in what the two apps
are for, and it is why every media app in that list is on it.

### High DPI and fractional scaling

Missed in the first draft, and it matters more here than in most apps because the entire surface is
resampled imagery.

Hyprland commonly runs fractional scale factors (1.25, 1.5), and Qt 6 reports that through
`devicePixelRatio` on a `fractional-scale-v1` surface. A thumbnail generated at logical pixel size
and drawn on a 1.5x surface is upscaled and visibly soft, which on a photo grid reads as a broken
app.

- Generate thumbnails at `ceil(logicalSize * devicePixelRatio)`, not at logical size.
- Key the cache on the rendered pixel size as well as path, mtime and size, so moving the window to
  a differently scaled monitor regenerates rather than upscales.
- Cap generation at 2x. Beyond that the cost is not repaid.
- Set `smooth: true` and `mipmap: true` on downscaled `Image` items, and always set
  `sourceSize` so Qt decodes at target size rather than decoding a 12-megapixel JPEG to draw a
  200px cell.
- Re-evaluate on `screenChanged`, since dragging between monitors changes the ratio live.

### Where the alpha comes from

Never hardcode it. The active theme decides, and a theme that wants an opaque surface gets one.

`omarchy-theme-set` materializes `shell.toml` into `~/.local/state/omarchy/current/theme/`, carrying
per-section surface and alpha tokens. omakade already reads the `launcher` section's alpha through
`readSectionAlpha()` and exposes it as `surfaceAlpha`. Lift that as-is.

`OmarchyTheme` already exposes everything the look needs, so almost nothing is invented:

```
surfaceAlpha    cornerRadius    gapsOut    fontFamily    mode
accent  selection  muted
background  darkBackground  darkerBackground  lighterBackground
foreground  darkForeground  lightForeground  brightForeground  mutedText
red  yellow  green  cyan  blue  magenta
```

Rules that follow from that list:

- **`cornerRadius` comes from the theme.** Omarchy's default is `0`. Do not hardcode rounded
  corners; a sharp theme must render sharp.
- **`fontFamily` comes from the theme.** No bundled UI font.
- **`gapsOut` informs padding** so spacing agrees with the window gaps around it.
- **Light themes are real.** `mode` is `light` or `dark`, and `themes/*/` ships a `light.mode`
  marker. Every surface, overlay scrim and matte chip must be derived from the palette, never from a
  literal that only works on a dark ground. Test against a light theme every milestone, not at the
  end.

### Seamless theme changes

The watcher gives correctness. Animation gives seamlessness.

`QFileSystemWatcher` on the theme directory fires `themeChanged`, and every color is a bound
`Q_PROPERTY`, so QML repaints on its own. Left alone that is a hard snap: the entire window changes
color in one frame, which reads as a glitch.

- Put `Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }` on the
  surface and text color bindings so a theme change cross-fades.
- Animate `surfaceAlpha` the same way.
- Do **not** animate `fontFamily` or `cornerRadius`; those snap, and animating them looks broken.
- Debounce the watcher. `omarchy-theme-set` writes several files into the staging directory and then
  moves it into place, so a naive watcher fires repeatedly. omakade already has `scheduleReload()`
  for this; keep it.
- Never rebuild the grid or drop thumbnail caches on a theme change. Only colors change.

The target: change theme with omaroll open and in view, and the window glides to the new palette
while scroll position, selection and every thumbnail stay exactly where they were.

### Acceptance

- Thumbnails sample pixel-identical to the source file with a color picker, on any theme
- Chrome alpha tracks `shell.toml`; an opaque theme renders fully opaque
- Thumbnails stay crisp at 1.0x, 1.25x and 1.5x scale, and re-sharpen when dragged between monitors
  of different scale
- Looks correct with Hyprland blur off, which is the default, and better with it on
- Corner radius follows the theme, `0` renders sharp
- Theme switch cross-fades in under 200ms with no relayout, no flicker, no lost scroll position
- Verified on one light theme and one dark theme every milestone

---

## 12. Milestones

Each one is independently shippable. Order matters, so it is numbered.

### v0.1: It sees your captures

Scaffold from the omakade skeleton. Theme, settings, single-instance and packaging all transfer on
day one.

- CMake project, Qt Quick shell, OmarchyTheme wired and following theme changes
- Surface alpha, corner radius and font pulled from the theme; cross-fade on theme change
- Verified against one light and one dark theme
- Screenshot and Recording sources with filename classification
- Static grid, day grouping, thumbnails
- Open in default handler, show in files

*Internal. The moment it is worth a screenshot.*

**Shipped.** One deliberate deviation: thumbnails go through `QImageReader` with a
scaled decode rather than libvips. For JPEG that already hands libjpeg a scale denominator, so a
large photo is never fully decoded to draw a tile, and it removed a dependency from v0.1 entirely.
`ThumbnailCache::renderImage` is the single function to swap when v0.2 measures against the
performance budget and finds libvips is worth the link. Video thumbnails came forward from v0.2
because `ffmpegthumbnailer` is a one-process call and a recordings grid without frames is not worth
looking at.

### v0.2: It is fast and complete  ·  **shipped**

- Video thumbnails via ffmpegthumbnailer, hover-scrub strips
- Thumbnails generated at `devicePixelRatio`, cache keyed on rendered pixel size
- Traversal rules: depth-1 captures, bounded recursion for Pictures/Videos, dotfiles skipped
- Remaining sources: Pictures, Videos, Downloads
- Thumbnail cache, background generation, on-screen priority
- Search, favorites, hide, sort
- `QFileSystemWatcher` live updates
- Full keyboard navigation

*Internal. Hits the performance budget here or it gets fixed here.*

### v0.3: It does things  ·  **shipped**

The action registry lands, and with it the whole delegation argument.

- ActionRegistry as data, ProcessLauncher with argv templates and no shell
- Trim to omacut, play in mpv, edit in pinta, view in imv
- ffmpeg one-shots: clip to GIF, resize, extract frame
- OCR to clipboard via tesseract, QR via zbarimg
- Send via localsend and omarchy-tailscale-send
- Multi-select and bulk actions
- Actionable errors when a handler is missing, naming the package to install

*First public beta. Post it to the Omarchy Discord.*

### v0.4: Mattes  ·  **shipped**

- HueExtractor and MatteComposer, all seven mattes, ported from compose.rs
- MattePicker sheet, six variants, keyboard select
- Padding and aspect presets, output size caps
- Composite to clipboard and to a new file beside the original

*The feature the launch video opens on.*

### v0.9: Release candidate  ·  **shipped**

- `--demo` mode with a deterministic fictional library, same as omakade, so launch material never
  exposes real files
- Settings screen: watched folders, default actions per kind, cache size
- Empty states, first-run state, permission and missing-folder handling
- Test pass, including classification against real-world filename patterns
- README, screenshots, demo video, project page

*Tagged RC, tested on a clean Omarchy VM.*

### v1.0: Ship  ·  **release candidate**

Code, packaging, tests and README are done. The old local `v1.0.0` tag predates
the release candidate and must be replaced at the final commit. Remaining work:

- GitHub release with `.pkg.tar.zst` and SHA256SUMS
- Launch post

### What actually shipped, against the plan

Two deviations, both recorded rather than quietly made:

- **Thumbnails use `QImageReader` scaled decode, not libvips.** For JPEG that
  already hands libjpeg a scale denominator, so a large photo is never fully
  decoded to draw a tile, and it removed a dependency. `ThumbnailCache::renderImage`
  is the one function to swap if measurement ever justifies the link.
- **Video thumbnails and hover-scrub came forward from v0.2 to v0.1.**
  `ffmpegthumbnailer` is a one-process call and a recordings grid without frames
  is not worth looking at.

Three features in the plan were **deleted after auditing the Omarchy tree**,
because something already installed does the job:

- Annotation → `tensaku-edit`, honouring `$OMARCHY_SCREENSHOT_EDITOR`
- GIF, resize and format conversion → `omarchy-transcode`
- Clipboard → `omarchy-clipboard-paste-file`

Added beyond the plan, because verification needed them and the launch will too:

- `--demo`, a deterministic fictional library, so no screenshot of omaroll ever
  shows a real file
- `--render <file> [--render-view grid|detail|video|slideshow|matte|settings]`, which grabs the scene
  graph rather than the screen, so a render cannot be spoiled by an overlapping
  window and the project's images are reproducible
- 32 tests over classification, traversal, live folders, albums, lifecycle, actions, clipboard safety,
  trash recovery, thumbnails and every matte, including the output-budget ceiling

---

## 13. Packaging and distribution

The exact path omakade already took, which means it is known to work.

Dependency set matched to what the first-party Qt apps actually declare. `omacut`, `omawrite` and
`omacalc` all depend on `hicolor-icon-theme` and `xdg-desktop-portal`, which the first draft of this
plan omitted, and none of them declare `qt6-wayland`.

```bash
pkgname=omaroll
pkgdesc='Everything you capture, in one place'
license=('MIT')
depends=('qt6-base' 'qt6-declarative' 'qt6-multimedia'
         'hicolor-icon-theme' 'xdg-desktop-portal'
         'libvips' 'ffmpegthumbnailer')
optdepends=('omacut: trim recordings'
            'tensaku: annotate screenshots'
            'tesseract: copy text out of a screenshot'
            'zbar: scan QR codes'
            'pinta: edit images'
            'mpv: play video'
            'imv: view images full size'
            'localsend: send to another device')
makedepends=('cmake' 'ninja' 'pkgconf')
```

`qt6-multimedia` is what omacut pulls for video playback and omaroll needs it for in-place preview.

Every optdepend is already installed on a stock Omarchy box, so the list functions as documentation
rather than as work for the user. Per Omarchy's own AGENTS.md, commands from the default package set
are runtime invariants and should be invoked directly without defensive presence checks. omaroll
still degrades gracefully on plain Arch via the AUR, but the check is at the action-registry level,
not scattered through the code: a handler whose binary is absent greys its row out and names the
package.

### Channels

- **GitHub release** with the prebuilt package and SHA256SUMS, verified install in three commands.
- **Omarchy repository** later, after upstream inclusion, for normal system updates.
- **AUR** is optional for Arch users outside Omarchy.
- Ship `.desktop` and `metainfo.xml` so it appears in the launcher properly.

### Desktop entry

Follow omacut's shape rather than omakade's. omacut declares a `MimeType` list and takes `%f`, which
is what lets other apps hand it a file, precisely how omaroll will invoke it. omaroll should be
equally openable, so a file manager or another tool can hand it a directory or an image.

```ini
[Desktop Entry]
Type=Application
Name=Omaroll
GenericName=Capture Library
Comment=Everything you capture, in one place
Exec=omaroll %f
Icon=io.github.tsouth89.omaroll
Terminal=false
Categories=Graphics;Viewer;AudioVideo;
MimeType=image/png;image/jpeg;image/webp;video/mp4;video/x-matroska;inode/directory;
Keywords=screenshot;recording;capture;gallery;library;
StartupNotify=true
StartupWMClass=io.github.tsouth89.omaroll
```

The reverse-DNS id matches the AppStream component and Omakade's packaging. It must match
`setDesktopFileName()` in `main.cpp`, `StartupWMClass`, and the Hyprland rule. Set it once and never
change it.

---

## 14. Inclusion in base Omarchy

Verified against the Omarchy tree rather than assumed. Build to this from day one; each item is
cheap now and expensive later.

### Blocking: license must be MIT

Every first-party Omarchy app is MIT: `omacut`, `omawrite`, `omacalc`, `aether`, `ttfx`, all under
`github.com/omacom-io`. The bundled third-party apps are MIT, MPL-2.0 (`tensaku`) or Apache-2.0
(`herdr`). **There is no GPL-licensed application in the bundled set.**

The first draft of this plan said GPL-3.0-or-later, inherited from omakade without checking. That is
a plausible adoption blocker, and relicensing later needs sign-off from every contributor who has
landed a patch by then. Ship as MIT from the first commit.

### Not blocking: the repo can stay under `tsouth89`

Inclusion does not require donating the project to the `omacom-io` org. Four bundled packages are
third-party repos owned by their authors:

| Package | Repo | License |
|---|---|---|
| `tensaku` | `jondkinney/tensaku` | MPL-2.0 |
| `cliamp` | `bjarneo/cliamp` | MIT |
| `herdr` | `herdrdev/herdr` | Apache-2.0 |
| `tobi-try` | `tobi/try` | MIT |

### The mechanism is a migration

Apps land in base through a timestamped migration plus a line in `install/omarchy-base.packages`.
The `omacalc` migration is the template, and it shows the replacement pattern too:

```bash
# migrations/1785637426.sh
echo "Replace GNOME Calculator with Omacalc"

omarchy-pkg-add omacalc
omarchy-pkg-drop gnome-calculator
```

omaroll's would be a two-line version with no drop, since it displaces nothing. Write it in advance
and keep it in `packaging/upstream/` so the eventual PR is a copy, not a design exercise.

### Menu entry

`default/omarchy/omarchy-menu.jsonc` already has a `trigger.capture` group holding Screenshot,
Screenrecord, Text, QR and Color. omaroll belongs there as a sibling:

```jsonc
"trigger.capture.library": {"icon":"󰋩","label":"Library","action":"uwsm-app -- omaroll"},
```

Two house rules apply: **do not add `aliases`** to a new entry (reserved for established alternate
names), and the icon must be a glyph the Omarchy icon font already carries.

### Conventions to build to now

- **Theme by watching state, not by upstream patch.** Covered in section 6. No `post_theme_commands`
  entry needed, which is one less file to touch in the PR.
- **Window rule.** omaroll tiles like a normal window, so it needs no float rule. It **does** need
  the media opacity opt-out: add `omaroll` to the "No transparency on media windows" list in
  `default/hypr/apps/system.lua` alongside mpv, imv and Pinta. See section 11 for why. That is a
  third line in the upstream PR, and until it lands the app should ship a `hypr/` snippet users can
  drop in.
- **App id.** `io.github.tsouth89.omaroll`, matching the AppStream component and Omakade's
  reverse-DNS form. On Wayland Qt derives `app_id` from `QGuiApplication::setDesktopFileName()`, so
  that call, the `.desktop` filename, `StartupWMClass` and the hypr rule must agree. Section 13.
- **Default state via `/etc/skel`.** Bundled apps seed defaults there; tensaku ships
  `/etc/skel/.local/state/tensaku/state.toml`. If omaroll needs shipped defaults, that is the path.
  Better: need none, per the zero-config principle.
- **`.desktop` + `metainfo.xml`** with a `hicolor` scalable icon, matching what omacut installs.
- **Use the house helpers**, not raw equivalents: `omarchy-clipboard-paste-file` over `wl-copy`,
  `omarchy-notification-send` over `notify-send`, `omarchy-transcode` over hand-rolled ffmpeg.
  Anything omaroll shells out to should be the same command the menu would run.

### Style rules for the upstream PR

These apply to the migration and menu entry contributed to the `omarchy` repo, not to omaroll's own
C++:

- `#!/bin/bash`, never `#!/usr/bin/env bash`
- Two-space indent, no tabs
- `[[ ]]` for string and file tests, `(( ))` for numeric
- No hard wrapping in markdown; break at structural boundaries only
- Atomic commits, one coherent change each
- `omarchy-pkg-add` rather than raw `pacman`

### Path to inclusion

1. Ship v1.0 on GitHub under MIT. Get real users.
2. Let it sit in OPR long enough to prove it is maintained and does not break on Omarchy updates.
3. Open one PR against `omarchy`: the package-list line, the migration, the menu entry. Small,
   reviewable, and matching every convention above.

Do not open that PR early. `tensaku` and `cliamp` were adopted because they were already good and
already used, not because someone proposed them.

---

## 15. Launch

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

## 16. Risks

| Risk | Mitigation |
|---|---|
| It reads as a worse file manager | No tree, no path bar, no columns. Day grouping, kind sections and a default action per kind are what make it a library. Main thing to protect in review. |
| Pictures and Videos sections overlap nautilus | They are secondary tabs. Screenshots and Recordings are the hero, and they are the two nautilus genuinely cannot help with. |
| Thumbnail generation on a huge library | Bounded queue, on-screen priority, persistent cache. Budget set at v0.2 and enforced there. |
| Classification misses non-Omarchy filenames | Pattern list covers grim, flameshot, OBS, gpu-screen-recorder and yt-dlp templates, falls back to extension, user-extendable. Beta at v0.3 exists to collect the misses. |
| Scope creep into editing | The out-list, and the rule that a request answerable with a package name becomes a matrix row. |
| Upstream ships something similar | Move fast and stay compatible rather than competitive. The matte is the part nothing else has, and delegating to omacut means an upstream improvement to omacut makes omaroll better rather than redundant. |
| License blocks adoption | Resolved: MIT from the first commit. See section 13. Relicensing after contributors arrive is the expensive version of this problem. |
| Duplicating something already in base | This already happened once in drafting: annotation looked like a gap and tensaku was already there. Before any native feature, grep `install/omarchy-base.packages` and `bin/` first. |
| Thumbnails render with wrong colors | The `default-opacity` tag dims every window by default. Without the media opt-out in section 11, every thumbnail renders at 96-98.5%. Ship the hypr snippet from day one and check with a color picker. |
| Looks like glass on a flat desktop | Omarchy runs blur off, rounding 0, shadows off. Read `cornerRadius` from the theme and design for blur being absent. |
| Soft thumbnails on fractional scaling | Generate at `devicePixelRatio` and key the cache on rendered pixel size. On a photo grid this is the difference between polished and broken, and 1.25x/1.5x are common on Hyprland. |
| App id drift | `setDesktopFileName()`, the `.desktop` filename, `StartupWMClass` and the Hyprland opacity rule must all agree. A mismatch silently breaks the opacity opt-out with no error. |

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
