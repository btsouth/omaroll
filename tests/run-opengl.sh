#!/usr/bin/env bash
set -euo pipefail

# Run inside xvfb-run in CI. Offscreen also works with a local Mesa display.
# The demo and UI suite create disposable profiles and media libraries.
build_dir=$(realpath "${1:-build/release}")
output_dir="$build_dir/opengl-renders"
mkdir -p "$output_dir"
export QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= QT_IM_MODULE=compose
export QT_QUICK_BACKEND=rhi QSG_RHI_BACKEND=opengl LIBGL_ALWAYS_SOFTWARE=1
export OMAROLL_REQUIRE_OPENGL=1
export QT_AUDIO_BACKEND=pulseaudio PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=omaroll-no-audio

timeout 180 "$build_dir/omaroll_ui_tests"
for view in grid video ocr; do
  rm -f -- "$output_dir/$view.png"
  timeout 30 "$build_dir/omaroll" --render "$output_dir/$view.png" \
    --render-view "$view" --render-size 560x420
  test "$(magick identify -format '%m %wx%h' "$output_dir/$view.png")" = 'PNG 560x420'
done
