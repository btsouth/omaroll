#!/usr/bin/env bash
set -euo pipefail
build=${1:-build/fuzz}
seconds=${2:-60}
mkdir -p "$build/fuzz-results"
# Leak checks stay on; only the documented external allocations are ignored.
export LSAN_OPTIONS="suppressions=$PWD/tests/lsan.supp${LSAN_OPTIONS:+:$LSAN_OPTIONS}"
status=0
for parser in launch ipc matroska pdf image raw subtitles organization; do
  corpus="$build/fuzz-results/$parser"
  mkdir -p "$corpus"
  cp tests/fuzz/corpus/"$parser"/* "$corpus/"
  "$build/fuzz_$parser" "$corpus" -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 -max_len=65536 \
    -artifact_prefix="$build/fuzz-results/$parser-" 2>"$build/fuzz-results/$parser.log" || {
    status=1
    echo "fuzz_$parser failed; last lines of its log:" >&2
    tail -n 40 "$build/fuzz-results/$parser.log" >&2
  }
  { grep -h '^Done' "$build/fuzz-results/$parser.log" || true; } | sed "s/^/fuzz_$parser: /"
done
exit "$status"
