# Omaroll

**Everything you capture, in one place.**

A fast, local-first capture library for Omarchy. Omaroll finds every screenshot,
screen recording, picture and download you already have, arranges them by day,
and hands each one to the tool that already owns the job.

It replaces nothing. It just gives all of it a home.

> Omaroll is an independent community project. It is not an official Omarchy
> application.

![The library, grouped by day](docs/library.png)

---

## Why

Omarchy ships an unusually good capture stack and then drops the output into a
folder. `omarchy-capture-screenshot`, `omarchy-capture-screenrecording`, omacut,
tensaku, pinta, tesseract, `omarchy-transcode` are all there and all good. What
is missing is the two steps after capture: finding the thing again, and doing
the obvious next thing with it.

```
$ ls ~/Videos
screenrecording-2026-08-30_20-10-45.mp4
screenrecording-2026-08-30_20-10-59.mp4
screenrecording-2026-08-30_20-11-49.mp4
...
```

Omaroll is the wire between the tools that make captures and the tools that act
on them.

## What it does

- **Sees everything you already have.** Screenshots, recordings, pictures,
  videos and downloads, grouped by day, newest first. No import, no setup.
- **Knows what each file is.** Omarchy stamps its captures with a predictable
  name, so a screenshot is a Screenshot even though it lives in `~/Pictures`
  next to every other image.
- **Hands off the work.** Trim goes to omacut, annotate to tensaku, convert to
  `omarchy-transcode`, play to mpv, text to tesseract. Fifteen of the sixteen
  actions are tools you already have installed.
- **Makes a screenshot postable.** The one thing Omaroll builds itself: six
  finished backgrounds derived from the image's own dominant colour. Pick one,
  it is on your clipboard and saved beside the original.
- **Follows your theme.** Reads the active Omarchy palette, font, corner radius
  and surface alpha, and cross-fades when you change theme. No restart.

## Actions

Every handler below already ships with Omarchy, with exactly one exception.

| Capture | Action | Handler |
|---|---|---|
| Recording | Trim *(default)* | `omacut` |
| Recording | Clip to GIF · Resize | `omarchy-transcode` |
| Recording | Play | `mpv` |
| Screenshot | **Make it postable** *(default)* | **native** |
| Screenshot | Annotate | `$OMARCHY_SCREENSHOT_EDITOR`, default `tensaku-edit` |
| Screenshot | Copy the text | `tesseract` |
| Image | Convert to JPEG | `omarchy-transcode` |
| Image | Edit · View | `pinta` · `imv` |
| Image | Scan QR code | `zbarimg` |
| Any | Copy to clipboard | `omarchy-clipboard-paste-file` |
| Any | Send to a device | `localsend` · `omarchy-tailscale-send` |
| Any | Show in files | `nautilus` |
| Any | Move to Trash | XDG trash, never `unlink` |

An action whose program is missing is shown greyed with the package to install,
rather than hidden.

![Every action for one capture](docs/detail.png)

## Mattes

![Six finished backgrounds, from the image's own colour](docs/matte.png)

Adaptive and Deep are gradients built from the capture's dominant hue, Aurora is
a soft mesh, Slate and Paper are neutrals, Pop is the complementary hue, and None
is the raw capture so the picker is never a tax. Deterministic per file: the same
screenshot always offers the same six.

The original is never touched. The composite is written as a new file beside it
and put on your clipboard.

## Keyboard

| Key | Does |
|---|---|
| arrows · `hjkl` | Move |
| `Space` | Preview with every action for that capture |
| `Enter` | Preview |
| `M` | Make it postable |
| `T` · `P` | Trim · Play a recording |
| `A` · `C` | Annotate · Copy the text |
| `Y` · `S` · `F` | Clipboard · Send · Show in files |
| `V` · `H` | Favourite · Hide |
| `X` · `Ctrl+A` | Select · Select all |
| `Del` | Move to Trash, with confirm |
| `/` · `R` | Search · Rescan |
| `Esc` | Clear selection, then close |

## Install

Requires Omarchy or Arch with Qt 6.8+.

```bash
git clone https://github.com/tsouth89/omaroll
cd omaroll
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

### Transparency

Omaroll paints its own translucent chrome from your theme and draws every
thumbnail fully opaque on top. Omarchy dims all windows slightly by default,
which would wash out the pictures, so Omaroll opts out the same way mpv, imv and
Pinta do. Drop `packaging/hypr/omaroll.lua` into `~/.config/hypr/` and require
it, or add to your Hyprland config:

```lua
o.window("^(omaroll)$", { tag = "-default-opacity" })
o.window("^(omaroll)$", { opacity = "1 1" })
```

## Try it without your own files

```bash
omaroll --demo
```

Builds a deterministic fictional library in a temp directory and browses that
instead. Nothing personal appears on screen, which is also how every screenshot
in this repository is made.

```bash
omaroll --render shot.png --render-view matte
```

Renders a view to a PNG and exits. It grabs the scene graph rather than the
screen, so an overlapping window cannot spoil the shot.

## What it will not do

The out-list is the more important half.

- **No video editing.** omacut owns it and is good.
- **No annotation.** tensaku ships in base and is already the screenshot editor.
- **No image editing.** pinta owns it.
- **No capture.** `omarchy-capture-*` owns it. Omaroll grabs no hotkeys and
  fights nothing for PrtScn.
- **No scrolling capture.** It needs synthetic input into another window, which
  Wayland blocks by design.
- **No file tree.** If it starts to look like a file manager, something has gone
  wrong.

If a feature request can be answered with the name of a package already in
`omarchy-base.packages`, the answer is a row in the action table, not a module in
the codebase.

## Design notes

- **Read-only on discovery.** Omaroll never moves, renames or rewrites a file it
  finds. Every action that produces output writes a new file beside the original.
- **Offline.** Nothing in the core path touches the network. No telemetry, no
  sync, no accounts.
- **High DPI.** Thumbnails are generated at `devicePixelRatio` and the cache is
  keyed on the rendered pixel size, so a 1.25x or 1.5x monitor gets crisp tiles
  rather than upscaled ones.
- **Cache and state.** Thumbnails in `~/.cache/omaroll/thumbs`, bounded at 256MB
  and pruned least-recently-used at launch. Settings in `~/.config/omaroll`.
  Both are safe to delete at any time.

## Development

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`PLAN.md` carries the full design rationale, the Omarchy conventions this
follows, and the path to inclusion in base Omarchy.

## License

MIT. See [LICENSE](LICENSE).
