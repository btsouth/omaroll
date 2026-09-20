#!/usr/bin/env bash
set -euo pipefail

# Run inside xvfb-run in CI. Offscreen also works with a local Mesa display.
# The demo and UI suite create disposable profiles and media libraries.
build_dir=$(realpath "${1:-build/release}")
output_dir="$build_dir/opengl-renders"
fixtures="$(cd "$(dirname "$0")/fixtures/themes" && pwd)"
mkdir -p "$output_dir"
export QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= QT_IM_MODULE=compose
export QT_QUICK_BACKEND=rhi QSG_RHI_BACKEND=opengl LIBGL_ALWAYS_SOFTWARE=1
export OMAROLL_REQUIRE_OPENGL=1
export QT_AUDIO_BACKEND=pulseaudio PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=omaroll-no-audio

timeout 180 "$build_dir/omaroll_ui_tests"

# Every render is its own process under a disposable profile, so it draws with
# the fixture's palette rather than whatever theme this machine happens to use.
# The profile is also what keeps the renders identical in CI, where no theme is
# installed at all.
render() { # theme view size
  local theme="$1" view="$2" size="$3"
  local profile="$build_dir/render-profile/$theme"
  if [[ ! -d "$profile/.local/state/omarchy/current/theme" ]]; then
    mkdir -p "$profile/.local/state/omarchy"
    cp -r "$fixtures/$theme" "$profile/.local/state/omarchy/current"
  fi
  local png="$output_dir/$theme-$view-$size.png"
  rm -f -- "$png"
  HOME="$profile" timeout 30 "$build_dir/omaroll" --render "$png" \
    --render-view "$view" --render-size "$size"
  test "$(magick identify -format '%m %wx%h' "$png")" = "PNG $size"
}

# Every view in the dark palette. The light palette then re-renders the surfaces
# that carry the most chrome, where a light background changes what is readable.
# Each render costs about twelve seconds of deliberate settling, so the matrix is
# worth keeping to the views that can actually differ.
for view in grid detail document video slideshow matte corrections compare export rename ocr duplicates browser settings; do
  render dark "$view" 1280x820
done
for view in grid detail document corrections compare duplicates browser settings; do
  render light "$view" 1280x820
done
# The smallest window the app allows, where the chrome has the least room. A
# document is rendered there in both palettes: that corner is where the rows
# collided and where a light background leaves a page nothing to sit against.
for view in grid detail video ocr; do
  render dark "$view" 560x420
done
render light document 560x420
