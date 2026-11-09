#!/usr/bin/env bash
# Runs every parser harness for a fixed time from its checked-in seeds.
#   tests/fuzz/run.sh <build> [seconds] [extra corpus root]
# An extra corpus root (the nightly job's cached corpus) is fuzzed too and
# grows in place. Inputs that reach code the seeds do not are copied to
# <build>/fuzz-results/new/<parser>; see tests/README.md for folding them in.
set -euo pipefail
build=${1:-build/fuzz}
seconds=${2:-60}
extra=${3:-}
parsers=(launch ipc matroska pdf image raw subtitles organization query storyboard doctext exifdate hyprland)
mkdir -p "$build/fuzz-results"
# Leak checks stay on; only the documented external allocations are ignored.
export LSAN_OPTIONS="suppressions=$PWD/tests/lsan.supp${LSAN_OPTIONS:+:$LSAN_OPTIONS}"
status=0
for parser in "${parsers[@]}"; do
  seeds=tests/fuzz/corpus/$parser
  corpus="$build/fuzz-results/$parser"
  mkdir -p "$corpus"
  cp "$seeds"/* "$corpus/"
  dirs=("$corpus")
  if [[ -n $extra ]]; then
    mkdir -p "$extra/$parser"
    dirs=("$extra/$parser" "$corpus")
  fi
  options=(-timeout=10 -rss_limit_mb=2048 -max_len=65536 -artifact_prefix="$build/fuzz-results/$parser-")
  "$build/fuzz_$parser" "${dirs[@]}" -max_total_time="$seconds" "${options[@]}" \
    2>"$build/fuzz-results/$parser.log" || {
    status=1
    echo "fuzz_$parser failed; last lines of its log:" >&2
    tail -n 40 "$build/fuzz-results/$parser.log" >&2
  }
  { grep -h '^Done' "$build/fuzz-results/$parser.log" || true; } | sed "s/^/fuzz_$parser: /"
  # What the run found beyond the seeds: merge into a copy of the seeds, then
  # drop the seeds again.
  new="$build/fuzz-results/new/$parser"
  rm -rf "$new"
  mkdir -p "$new"
  cp "$seeds"/* "$new/"
  "$build/fuzz_$parser" -merge=1 "${options[@]}" "$new" "${dirs[@]}" \
    2>>"$build/fuzz-results/$parser.log" || true
  for seed in "$seeds"/*; do rm -f "$new/$(basename "$seed")"; done
  echo "fuzz_$parser: $(find "$new" -type f | wc -l) new interesting input(s)"
done
exit "$status"
