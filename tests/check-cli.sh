#!/usr/bin/env bash
# Command-line checks that need the real binary: --set-default's summary,
# --diagnostics without a display, the window rules commands, and the man page
# matching --help and installing. Everything runs against scratch HOME, config
# and data directories and a fake xdg-mime, never the real desktop.
#
# Usage: check-cli.sh <omaroll binary> <build dir> <source dir>
set -euo pipefail

omaroll=$1
build=$2
source=$3
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

fail() { echo "check-cli: $*" >&2; exit 1; }

export HOME=$scratch/home
export XDG_CONFIG_HOME=$scratch/config
export XDG_DATA_HOME=$scratch/data
export XDG_DATA_DIRS=$scratch/system
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME/applications" "$scratch/system" "$scratch/bin"
cp "$source/packaging/io.github.tsouth89.omaroll.desktop" "$XDG_DATA_HOME/applications/"

# xdg-mime that answers imv or mpv and records what it is asked to set.
cat > "$scratch/bin/xdg-mime" <<'EOF'
#!/bin/sh
if [ "$1" = query ]; then
  case "$3" in
    video/*|application/vnd.ms-asf) echo mpv.desktop ;;
    application/pdf) echo org.gnome.Papers.desktop ;;
    *) echo imv.desktop ;;
  esac
  exit 0
fi
[ "$1" = default ] && echo "$3" >> "$XDG_CONFIG_HOME/set.log"
exit 0
EOF
chmod +x "$scratch/bin/xdg-mime"
export PATH=$scratch/bin:$PATH

# --set-default: one line and the undo, not a line per type.
out=$("$omaroll" --set-default)
[[ $(wc -l <<<"$out") -eq 2 ]] || fail "--set-default printed more than two lines: $out"
grep -Eq '^Omaroll now opens [0-9]+ types of picture and video, replacing imv and mpv\.$' <<<"$out" ||
  fail "unexpected --set-default summary: $out"
grep -qx 'To undo: omaroll --unset-default' <<<"$out" || fail "no undo line: $out"
[[ $(wc -l < "$XDG_CONFIG_HOME/set.log") -gt 20 ]] || fail "xdg-mime was not asked to set the types"

out=$("$omaroll" --set-default --verbose)
grep -q '^image/png: imv.desktop -> io.github.tsouth89.omaroll.desktop$' <<<"$out" ||
  fail "--verbose did not list the types: $out"
grep -qx 'To undo: omaroll --unset-default' <<<"$out" || fail "no undo line with --verbose"

out=$("$omaroll" --set-default pdf)
grep -qx 'Omaroll now opens 1 type of PDF, replacing Papers.' <<<"$out" || fail "pdf summary: $out"
grep -qx 'To undo: omaroll --unset-default pdf' <<<"$out" || fail "pdf undo line: $out"

# --diagnostics: no display needed, and the facts a bug report asks for.
out=$(QT_QPA_PLATFORM=no-such-platform WAYLAND_DISPLAY= DISPLAY= "$omaroll" --diagnostics)
for field in 'Omaroll: ' 'Qt: ' 'Platform: none (command line)' 'Omarchy: ' 'Hyprland: ' 'GPU: ' \
             'Theme: ' 'tesseract: ' 'ffmpeg: '; do
  grep -q "^$field" <<<"$out" || fail "--diagnostics lacks '$field': $out"
done
grep -q "$scratch" <<<"$out" && fail "--diagnostics printed a path: $out"

# Window rules: refused without an Omarchy config, then in and out cleanly.
if "$omaroll" --install-window-rules 2>/dev/null; then fail "installed rules without a config"; fi
mkdir -p "$HOME/.config/hypr"
printf 'require("default.hypr.omarchy")\n' > "$HOME/.config/hypr/hyprland.lua"
cp "$HOME/.config/hypr/hyprland.lua" "$scratch/hyprland.before"
"$omaroll" --install-window-rules | grep -q 'installed' || fail "install did not report"
[[ -f $HOME/.config/hypr/omaroll.lua ]] || fail "no rules file"
grep -q 'hypr.omaroll' "$HOME/.config/hypr/hyprland.lua" || fail "rules not required"
"$omaroll" --remove-window-rules | grep -q 'removed' || fail "remove did not report"
[[ ! -e $HOME/.config/hypr/omaroll.lua ]] || fail "rules file left behind"
cmp -s "$scratch/hyprland.before" "$HOME/.config/hypr/hyprland.lua" || fail "hyprland.lua not restored"

# The man page covers every option in --help, and installs.
man=$source/packaging/omaroll.1
for option in $("$omaroll" --help | grep -oE '(^|[[:space:]])--[a-z][a-z-]+' | tr -d ' ' | sort -u); do
  grep -qF -- "$(sed 's/-/\\-/g' <<<"$option")" "$man" || fail "man page lacks $option"
done
if command -v groff >/dev/null; then
  warnings=$(groff -man -ww -z "$man" 2>&1 || true)
  [[ -z $warnings ]] || fail "groff warnings: $warnings"
fi
cmake --install "$build" --prefix "$scratch/prefix" >/dev/null
[[ -f $scratch/prefix/share/man/man1/omaroll.1 ]] || fail "man page not installed"
[[ -f $scratch/prefix/share/omaroll/hypr/omaroll.lua ]] || fail "rules file not installed"
# The installed README points at the repository for pages and images it lacks.
readme=$scratch/prefix/share/doc/omaroll/README.md
if grep -nE '\]\((docs|tests)/|src="docs/' "$readme"; then fail "installed README has relative links"; fi

echo "check-cli: ok"
