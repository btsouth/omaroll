#!/usr/bin/env bash
# Disposable Arch container only. Desktop hosts use tests/run-isolated.sh.
set -euo pipefail
mode=${1:?fuzz, tsan or coverage}
build=build/$mode
case "$mode" in
  fuzz)
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DOMAROLL_FUZZ=ON
    cmake --build "$build" --parallel 4 --target fuzz_launch fuzz_ipc fuzz_matroska fuzz_pdf fuzz_image fuzz_raw fuzz_subtitles fuzz_organization
    bash tests/fuzz/run.sh "$build" "${2:-60}"
    ;;
  tsan)
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      '-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer' -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
    cmake --build "$build" --parallel 4 --target omaroll_tests
    QT_QPA_PLATFORM=offscreen TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/tests/tsan.supp" \
      ctest --test-dir "$build" -R '^omaroll_tests$' --output-on-failure
    ;;
  coverage)
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      '-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping' -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate
    cmake --build "$build" --parallel 4
    mkdir -p "$build/profiles"
    LLVM_PROFILE_FILE="$PWD/$build/profiles/%p-%m.profraw" ctest --test-dir "$build" --parallel 4 --output-on-failure
    llvm-profdata merge -sparse "$build"/profiles/*.profraw -o "$build/coverage.profdata"
    objects=()
    for binary in "$build"/omaroll*tests "$build"/omaroll; do
      [[ -f $binary && -x $binary ]] && objects+=(-object "$binary")
    done
    llvm-cov report "${objects[@]}" -instr-profile="$build/coverage.profdata" -ignore-filename-regex='(tests/|_autogen/|/usr/)' | tee "$build/summary.txt"
    llvm-cov show "${objects[@]}" -instr-profile="$build/coverage.profdata" -ignore-filename-regex='(tests/|_autogen/|/usr/)' -format=html -output-dir="$build/html"
    ;;
  *) exit 2 ;;
esac
