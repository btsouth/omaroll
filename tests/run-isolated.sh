#!/usr/bin/env bash
set -euo pipefail

# Local Linux checks: retain graphics access, but hide the session audio sockets
# and ALSA devices. Mount changes apply only to this process's namespace.
build_dir=$(realpath "${1:-build/release}")
if (($#)); then shift; fi
if (($# == 0)); then
  set -- ctest --test-dir "$build_dir" --output-on-failure
fi
command -v bwrap >/dev/null || { echo 'Install bubblewrap to run isolated local checks.' >&2; exit 1; }
args=(--ro-bind / / --dev-bind /dev /dev --tmpfs /tmp
      --tmpfs "/run/user/$(id -u)" --bind "$build_dir" "$build_dir"
      --unshare-net --unshare-pid --proc /proc --die-with-parent --new-session
      --setenv XDG_RUNTIME_DIR /tmp/omaroll-runtime
      --setenv QT_QPA_PLATFORM offscreen
      --setenv QT_QPA_PLATFORMTHEME ""
      --setenv QT_AUDIO_BACKEND pulseaudio
      --setenv PULSE_SERVER unix:/nonexistent
      --setenv PIPEWIRE_REMOTE omaroll-no-audio)
if [[ -d /dev/snd ]]; then args+=(--tmpfs /dev/snd); fi
if [[ -d /tmp/.X11-unix ]]; then
  args+=(--ro-bind /tmp/.X11-unix /tmp/.X11-unix)
fi
exec bwrap "${args[@]}" /bin/bash -c 'mkdir -m 700 "$XDG_RUNTIME_DIR"; exec "$@"' bash "$@"
