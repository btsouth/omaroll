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
timeout 120 "$build_dir/omaroll_viewer_tests"

# Every render is its own process under a disposable profile, so it draws with
# the fixture's palette rather than whatever theme this machine happens to use.
# The profile is also what keeps the renders identical in CI, where no theme is
# installed at all. Each render gets a profile of its own, so they can run side
# by side: most of a render is deliberate settling, not work.
render() { # theme view size
  local theme="$1" view="$2" size="$3"
  local profile="$build_dir/render-profile/$theme-$view-$size"
  rm -rf -- "$profile"
  mkdir -p "$profile/.local/state/omarchy"
  cp -r "$fixtures/$theme" "$profile/.local/state/omarchy/current"
  local png="$output_dir/$theme-$view-$size.png"
  rm -f -- "$png"
  HOME="$profile" timeout 60 "$build_dir/omaroll" --render "$png" \
    --render-view "$view" --render-size "$size" >"$profile/render.log" 2>&1 || {
    echo "render failed: $theme $view $size" >&2
    cat "$profile/render.log" >&2
    return 1
  }
  if [[ "$(magick identify -format '%m %wx%h' "$png")" != "PNG $size" ]]; then
    echo "render has the wrong format or size: $png" >&2
    return 1
  fi
}

# Every view in the dark palette. The light palette then re-renders the surfaces
# that carry the most chrome, where a light background changes what is readable.
# The smallest window the app allows, where the chrome has the least room, gets
# the views that collided there, and a document in both palettes.
renders=()
for view in grid detail document video slideshow matte corrections compare export rename ocr duplicates browser settings viewer viewer-video viewer-info viewer-menu; do
  renders+=("dark $view 1280x820")
done
for view in grid detail document corrections compare duplicates browser settings viewer viewer-info viewer-menu; do
  renders+=("light $view 1280x820")
done
for view in grid detail video ocr document viewer viewer-video; do
  renders+=("dark $view 560x420")
done
renders+=("light document 560x420")

jobs="${OMAROLL_RENDER_JOBS:-$(( $(nproc) * 3 ))}"
failed=0
running=0
for spec in "${renders[@]}"; do
  # shellcheck disable=SC2086
  render $spec &
  running=$((running + 1))
  if (( running >= jobs )); then
    wait -n || failed=1
    running=$((running - 1))
  fi
done
while (( running > 0 )); do
  wait -n || failed=1
  running=$((running - 1))
done
(( failed == 0 ))
echo "rendered ${#renders[@]} views"
