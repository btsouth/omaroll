# omaroll

Media library and viewer for Omarchy. **v1.0.0 is a tested release candidate.**
See PLAN.md for the design rationale and README.md for what it does.

Build: `cmake -S . -B build -G Ninja && cmake --build build`
Test:  `ctest --test-dir build --output-on-failure` (32 tests)
Look:  `./build/omaroll --demo` or `--render shot.png --render-view matte`
       (`--render-view` takes grid, detail, video, slideshow, matte or settings; `OMAROLL_RENDER_PLATFORM=wayland`
       renders on the real stack, which is the only way to see video frames)

## Hard rules

- **GitHub account: always `tsouth89`.** Never `bts-cssi`. Check `gh auth status` before any `gh`
  command here; switch with `gh auth switch -u tsouth89` if it is not active.
- **GitHub publication is authorized.** Keep the repository and release under `tsouth89`. Do not
  post to social accounts without a separate explicit request.
- **License is MIT.** Not GPL. Every bundled Omarchy app is MIT/MPL/Apache; there is no GPL app in
  the base set, and relicensing after contributors arrive needs all of them to agree. PLAN.md §14.

## Before adding any feature

Grep `~/Projects/omarchy/install/omarchy-base.packages` and `~/Projects/omarchy/bin/` first. If a
package or `omarchy-*` command already does it, the answer is a row in the action matrix, not a
module. This has already caught three would-be features:

- Annotation → `tensaku-edit`, honoring `$OMARCHY_SCREENSHOT_EDITOR`
- Convert, resize, GIF → `omarchy-transcode <path> <format> <resolution>`. Write no ffmpeg.
- Copy to clipboard → `omarchy-clipboard-paste-file --copy-only`

Use house helpers over raw equivalents: `omarchy-notification-send`, not `notify-send`.

## Look and feel

- **Opt out of `default-opacity`** (`tag = "-default-opacity"`, `opacity = "1 1"`). Every media app
  in Omarchy does. Compositor dimming would wash out thumbnails.
- **In-app alpha on chrome only**, exactly like omakade: `ApplicationWindow { color: "transparent" }`
  plus one gradient painted at `Theme.surfaceAlpha` (from `shell.toml` `[launcher] background-alpha`,
  default 0.82). Thumbnails, previews and text are children drawn fully opaque on top.
- **Thumbnails at `devicePixelRatio`.** Cache keyed on rendered pixel size. Fractional scaling
  (1.25x, 1.5x) is common on Hyprland and logical-size thumbnails look soft.
- **App id is `io.github.tsouth89.omaroll`.** `setDesktopFileName()`, the `.desktop` filename,
  `StartupWMClass` and the hypr opacity rule must all agree or the opt-out silently fails.
- **Alpha, corner radius and font come from the theme**, never hardcoded. Omarchy defaults to
  rounding 0, blur off, shadows off. Design flat and sharp.
- **Cross-fade theme changes** with `Behavior on color`. Never relayout or drop caches on retint.
- Test one light theme and one dark theme every milestone.

## Stack

- C++20 + Qt 6.8 Quick + CMake/Ninja, matching omakade and the Omarchy first-party apps.
- Reuse the omakade skeleton (`~/Projects/steam-launcher`); lift table in PLAN.md §6.
- Theme by watching `~/.local/state/omarchy/current/theme/`. Do not add a `post_theme_commands`
  entry upstream; self-watching is correct and needs no upstream patch.
- Never move, rename or rewrite a user's capture file. Actions write new files beside originals.

## Verifying a change

Never drive the GUI by hand to check something; the compositor steals focus and
the shot comes out wrong. Use the render mode, which grabs the scene graph:

```bash
./build/omaroll --render /tmp/check.png --render-view grid   # or detail, matte
```

It runs against `--demo` automatically, so a render never contains a real file, and it draws
on the offscreen platform so the tiling compositor cannot resize the window under it. Pass
`--render-size WxH` for a specific size; the default is 1280x820.
