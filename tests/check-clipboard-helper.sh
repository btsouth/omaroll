#!/usr/bin/env bash
# The real clipboard helper (`omaroll --serve-clipboard`) against a compositor
# with ext-data-control: both types offered, the PNG whole, the JPEG bytes
# unchanged, the helper working from /, a second copy replacing the first
# helper, and the helper leaving once something else is copied.
#
# It takes over the session's clipboard, so it runs only when asked to, in a
# throwaway desktop such as omabox:
#   OMAROLL_CLIPBOARD_TEST=1 bash tests/check-clipboard-helper.sh <omaroll> <source dir>
# Anywhere else it skips (exit 77).
set -euo pipefail
omaroll=$(realpath -- "${1:?omaroll binary}")
source=$(realpath -- "${2:?source dir}")
skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*" >&2; exit 1; }
[[ ${OMAROLL_CLIPBOARD_TEST:-} == 1 ]] || skip "set OMAROLL_CLIPBOARD_TEST=1 in a throwaway desktop"
[[ -n ${WAYLAND_DISPLAY:-} ]] || skip "no Wayland display"
command -v wl-paste >/dev/null && command -v wl-copy >/dev/null || skip "wl-clipboard is not installed"

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
first=$scratch/first.jpg
second=$scratch/second.jpg
cp "$source/resources/demo/alpine-dawn.jpg" "$first"
cp "$source/resources/demo/black-sand-coast.jpg" "$second"
helper() { pgrep -f -- "--serve-clipboard image/jpeg $1\$" || true; }
gone() {
  for _ in $(seq 50); do
    kill -0 "$1" 2>/dev/null || return 0
    sleep 0.1
  done
  return 1
}
be16() { od -An -tu4 --endian=big -j "$2" -N4 "$1" | tr -d ' '; }

status=0
(cd "$scratch" && "$omaroll" --serve-clipboard image/jpeg "$first") || status=$?
((status != 3)) || skip "this compositor has no ext-data-control"
((status == 0)) || fail "the helper exited $status"

types=$(wl-paste --list-types)
[[ $types == $'image/jpeg\nimage/png' ]] || fail "offered types: $types"
wl-paste --type image/jpeg | cmp -s - "$first" || fail "the JPEG bytes changed"
wl-paste --type image/png > "$scratch/first.png"
[[ $(head -c 8 "$scratch/first.png" | od -An -tx1 | tr -d ' \n') == 89504e470d0a1a0a ]] ||
  fail "image/png is not a PNG ($(stat -c %s "$scratch/first.png") bytes)"
[[ $(tail -c 8 "$scratch/first.png" | od -An -tx1 | tr -d ' \n') == 49454e44ae426082 ]] ||
  fail "image/png is cut off"
[[ $(be16 "$scratch/first.png" 16)x$(be16 "$scratch/first.png" 20) == 1536x1024 ]] ||
  fail "image/png is $(be16 "$scratch/first.png" 16)x$(be16 "$scratch/first.png" 20)"
if command -v magick >/dev/null; then
  magick identify "$scratch/first.png" >/dev/null || fail "image/png does not decode"
fi

pid=$(helper "$first")
[[ -n $pid && $pid != *$'\n'* ]] || fail "expected one helper for the first copy, found: ${pid:-none}"
[[ $(readlink "/proc/$pid/cwd") == / ]] || fail "the helper works from $(readlink "/proc/$pid/cwd")"

"$omaroll" --serve-clipboard image/jpeg "$second" || fail "the second copy failed"
gone "$pid" || fail "the first helper is still running after the second copy"
wl-paste --type image/jpeg | cmp -s - "$second" || fail "the second copy is not on the clipboard"
second_pid=$(helper "$second")
[[ -n $second_pid ]] || fail "no helper for the second copy"
[[ -z $(pgrep -f -- "--serve-clipboard image/jpeg $first\$" || true) ]] || fail "helpers piled up"

echo text | wl-copy
gone "$second_pid" || fail "the helper is still running after a text copy"
echo "PASS: clipboard helper"
