#!/usr/bin/env bash
set -euo pipefail
build=${1:-build/fuzz}
seconds=${2:-60}
mkdir -p "$build/fuzz-results"
for parser in launch ipc matroska pdf image raw subtitles organization; do
  corpus="$build/fuzz-results/$parser"
  mkdir -p "$corpus"
  cp tests/fuzz/corpus/"$parser"/* "$corpus/"
  "$build/fuzz_$parser" "$corpus" -max_total_time="$seconds" -timeout=10 -max_len=65536 \
    -artifact_prefix="$build/fuzz-results/$parser-" 2>"$build/fuzz-results/$parser.log"
done
