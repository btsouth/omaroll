#!/usr/bin/env bash
set -euo pipefail

# Fails when a binary needs a newer Qt or glibc than Omarchy's stable channel
# ships. Stable and rc lag behind Arch, so such a package would install there
# and then fail to start.

binary="${1:?usage: check-omarchy-stable.sh BINARY}"
mirror="https://stable-mirror.omarchy.org"

# Major.minor of a package in the stable mirror, e.g. 6.11 for 6.11.2-3.
stable_version() {
  curl -fsSL "$mirror/$1/os/x86_64/$1.db" |
    tar -xzO --wildcards "$2-[0-9]*/desc" |
    sed -n '/^%VERSION%$/{n;p}' | sed 's/^[0-9]*://; s/[^0-9.].*//' | cut -d . -f 1,2
}

# Newest symbol version the binary needs with this prefix, e.g. 6.12 for Qt_6.
needed_version() {
  objdump -T "$binary" | grep -o "($1[0-9.]*)" | tr -d '()' | sed "s/^$1//" |
    sort -V | tail -n 1
}

check() {
  local name="$1" prefix="$2" stable="$3" needed
  needed="$(needed_version "$prefix")"
  test -n "$stable" || { echo "Could not read $name from $mirror" >&2; exit 1; }
  if [ -n "$needed" ] &&
    [ "$(printf '%s\n' "$needed" "$stable" | sort -V | tail -n 1)" != "$stable" ]; then
    echo "$binary needs $name $needed, but Omarchy stable has $name $stable" >&2
    exit 1
  fi
  echo "$name: needs ${needed:-nothing}, Omarchy stable has $stable"
}

check Qt Qt_ "$(stable_version extra qt6-base)"
check glibc GLIBC_ "$(stable_version core glibc)"
