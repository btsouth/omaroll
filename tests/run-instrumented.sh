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
    # Some core tests launch the omaroll executable itself.
    cmake --build "$build" --parallel 4 --target omaroll omaroll_tests
    # Arch's Qt is not built with TSan. ignore_noninstrumented_modules drops
    # reports raised by malloc/memcpy-style interceptors called from inside
    # uninstrumented libraries (Qt's own containers and thread pool), whose
    # locking TSan cannot see. Races in our code are still reported.
    QT_QPA_PLATFORM=offscreen \
      TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:ignore_noninstrumented_modules=1:suppressions=$PWD/tests/tsan.supp" \
      ctest --test-dir "$build" -R '^omaroll_tests$' --output-on-failure
    ;;
  coverage)
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      '-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping' -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate
    cmake --build "$build" --parallel 4
    mkdir -p "$build/profiles"
    rm -f "$build"/profiles/*.profraw
    test_status=0
    LLVM_PROFILE_FILE="$PWD/$build/profiles/%p-%m.profraw" ctest --test-dir "$build" --parallel 4 --output-on-failure || test_status=$?
    llvm-profdata merge -sparse "$build"/profiles/*.profraw -o "$build/coverage.profdata"
    objects=()
    for binary in "$build"/omaroll*tests "$build"/omaroll; do
      [[ -f $binary && -x $binary ]] && objects+=(-object "$binary")
    done
    llvm-cov report "${objects[@]}" -instr-profile="$build/coverage.profdata" -ignore-filename-regex='(tests/|_autogen/|/usr/|(^|/)build/)' | tee "$build/summary.txt"
    llvm-cov show "${objects[@]}" -instr-profile="$build/coverage.profdata" -ignore-filename-regex='(tests/|_autogen/|/usr/|(^|/)build/)' -format=html -output-dir="$build/html"
    # Coverage is a report, not a gate: the build-and-test jobs gate the
    # suites, and timing-sensitive tests are slower under instrumentation.
    if ((test_status != 0)); then
      echo "::warning::Some tests failed in the coverage build (ctest exit $test_status); see the log above."
    fi
    ;;
  *) exit 2 ;;
esac
