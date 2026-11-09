#!/usr/bin/env bash
# Command-line checks that need the real binary: --set-default's summary,
# --diagnostics without a display, the window rules commands, and the man page
# matching --help and installing. Everything runs against scratch HOME, config
# and data directories and a fake xdg-mime, never the real desktop.
#
# Usage: check-cli.sh <omaroll binary> <build dir> <source dir> [mandir] [datarootdir]
# The last two are CMake's CMAKE_INSTALL_MANDIR and CMAKE_INSTALL_DATAROOTDIR.
set -euo pipefail

omaroll=$1
build=$2
source=$3
mandir=${4:-share/man}
datarootdir=${5:-share}
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
if [ "$1" = default ]; then
  [ "$3" = "${FAIL_TYPE:-}" ] && exit 1
  echo "$3" >> "$XDG_CONFIG_HOME/set.log"
fi
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

# One type failing still prints the undo for the ones that changed.
if out=$(FAIL_TYPE=image/png "$omaroll" --set-default images 2>"$scratch/err"); then
  fail "--set-default succeeded with a failed type"
fi
grep -qx 'To undo: omaroll --unset-default images' <<<"$out" || fail "no undo after partial failure: $out"
grep -qx 'Could not set image/png' "$scratch/err" || fail "failed type not reported: $(cat "$scratch/err")"

# --diagnostics: no display needed, and the facts a bug report asks for.
out=$(QT_QPA_PLATFORM=no-such-platform WAYLAND_DISPLAY= DISPLAY= "$omaroll" --diagnostics)
for field in 'Omaroll: ' 'Qt: ' 'Platform: none (command line)' 'Omarchy: ' 'Hyprland: ' 'GPU: ' \
             'Theme: ' 'VA-API: ' 'tesseract: ' 'ffmpeg: '; do
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

# --set-default offers the rules while they would help, and not once they are
# installed or the offer was turned down in the library.
offer='To float the viewer and keep it opaque: omaroll --install-window-rules'
out=$("$omaroll" --set-default images)
grep -qxF "$offer" <<<"$out" || fail "--set-default did not offer the window rules: $out"
"$omaroll" --install-window-rules >/dev/null
out=$("$omaroll" --set-default images)
grep -qF 'install-window-rules' <<<"$out" && fail "offered rules that are installed: $out"
"$omaroll" --remove-window-rules >/dev/null
mkdir -p "$XDG_CONFIG_HOME/omaroll"
printf '[hyprland]\nwindowRulesOfferDismissed=true\n' > "$XDG_CONFIG_HOME/omaroll/omaroll.conf"
out=$("$omaroll" --set-default images)
grep -qF 'install-window-rules' <<<"$out" && fail "offered rules after Not now: $out"
rm "$XDG_CONFIG_HOME/omaroll/omaroll.conf"

# The man page covers every option in --help, and installs.
man=$source/packaging/omaroll.1
for option in $("$omaroll" --help | grep -oE '(^|[[:space:]])--[a-z][a-z-]+' | tr -d ' ' | sort -u); do
  grep -qF -- "$(sed 's/-/\\-/g' <<<"$option")" "$man" || fail "man page lacks $option"
done
# --help and the man page name exactly the render views main.cpp accepts.
views() { tr ',' '\n' | sed -E 's/ or /\n/g' | tr -d ' .' | grep -v '^$' | sort -u; }
accepted=$(sed -n '/renderViews = {/,/};/p' "$source/src/app/main.cpp" | grep -oE 'QStringLiteral\("[a-z-]+"\)' |
  sed -E 's/.*\("(.*)"\)/\1/' | sort -u)
helped=$("$omaroll" --help | sed -n '/--render-view/,/--render-size/p' | sed '$d' |
  sed -E 's/.*Which view to render: //; s/^ +//' | tr '\n' ' ' | views)
manned=$(sed -n '/^Which view to render:/,/^\.TP/p' "$man" | sed '$d' |
  sed -E 's/^Which view to render: //' | tr '\n' ' ' | views)
[[ -n $accepted && $accepted == "$helped" ]] || fail "--help render views differ: $(diff <(echo "$accepted") <(echo "$helped"))"
[[ $accepted == "$manned" ]] || fail "man page render views differ: $(diff <(echo "$accepted") <(echo "$manned"))"
if command -v groff >/dev/null; then
  warnings=$(groff -man -ww -z "$man" 2>&1 || true)
  [[ -z $warnings ]] || fail "groff warnings: $warnings"
fi
# Under DESTDIR, so an absolute install directory stays in scratch too.
DESTDIR=$scratch/dest cmake --install "$build" --prefix /prefix >/dev/null
installed() { [[ $1 == /* ]] && echo "$scratch/dest$1" || echo "$scratch/dest/prefix/$1"; }
[[ -f $(installed "$mandir")/man1/omaroll.1 ]] || fail "man page not installed"
[[ -f $(installed "$datarootdir")/omaroll/hypr/omaroll.lua ]] || fail "rules file not installed"
# The installed README points at the repository for pages and images it lacks.
readme=$(installed "$datarootdir")/doc/omaroll/README.md
if grep -nE '\]\((docs|tests)/|src="docs/' "$readme"; then fail "installed README has relative links"; fi

echo "check-cli: ok"
