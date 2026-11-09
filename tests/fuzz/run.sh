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
# Every finding becomes a regression test (tests/README.md), so its input is
# printed where it cannot expire with an artifact: base64 in the log and, on
# GitHub, in the job summary. Inputs are at most 64 KiB (-max_len). CI also uploads the files as fuzz-crash-inputs.
report_findings() {
  local parser=$1 input
  for input in "$build/fuzz-results/$parser"-{crash,leak,timeout,oom}-*; do
    [[ -f $input ]] || continue
    {
      echo "fuzz_$parser finding: $(basename "$input") ($(stat -c %s "$input") bytes)"
      echo "reproduce: build/fuzz/fuzz_$parser <input>; base64 of the whole input:"
      base64 -w 76 "$input"
    } | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}" >&2
  done
}
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
    report_findings "$parser"
  }
  { grep -h '^Done' "$build/fuzz-results/$parser.log" || true; } | sed "s/^/fuzz_$parser: /"
  # What the run found beyond the seeds: merge into a copy of the seeds, then
  # drop the seeds again.
  new="$build/fuzz-results/new/$parser"
  rm -rf "$new"
  mkdir -p "$new"
  cp "$seeds"/* "$new/"
  if ! "$build/fuzz_$parser" -merge=1 "${options[@]}" "$new" "${dirs[@]}" \
    2>"$build/fuzz-results/$parser-merge.log"; then
    status=1
    echo "fuzz_$parser: merging new inputs failed; last lines of its log:" >&2
    tail -n 20 "$build/fuzz-results/$parser-merge.log" >&2
    continue
  fi
  for seed in "$seeds"/*; do rm -f "$new/$(basename "$seed")"; done
  echo "fuzz_$parser: $(find "$new" -type f | wc -l) new interesting input(s)"
done
exit "$status"
