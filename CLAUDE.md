# omaroll

Capture library for Omarchy. See PLAN.md for the full spec.

## Hard rules

- **GitHub account: always `tsouth89`.** Never `bts-cssi`. Check `gh auth status` before any `gh`
  command here; switch with `gh auth switch -u tsouth89` if it is not active.
- **Nothing public until told otherwise.** No remote, no push, no release, no posts. Local only.
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
- **In-app alpha on chrome only.** Thumbnails, previews and text are always fully opaque.
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
