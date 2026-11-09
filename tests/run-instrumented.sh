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
    # Some core tests launch the omaroll executable. Give them an uninstrumented
    # one: this job checks the core suite, not the whole GUI app under TSan.
    cmake -S . -B build/tsan-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    cmake --build build/tsan-app --parallel 4 --target omaroll
    cp build/tsan-app/omaroll "$build/omaroll"
    # Arch's Qt and glibc are not built with TSan, so their internal locking is
    # invisible to it. Collect every report, then let tests/tsan-triage.py fail
    # only on reports whose accesses start in instrumented code.
    # (ignore_noninstrumented_modules is not usable: it also hides Qt's own
    # thread joins, and TSan then aborts when a thread id is reused.)
    rm -f "$build"/tsan-report.*
    test_status=0
    QT_QPA_PLATFORM=offscreen \
      TSAN_OPTIONS="halt_on_error=0:exitcode=0:second_deadlock_stack=1:log_path=$PWD/$build/tsan-report:suppressions=$PWD/tests/tsan.supp" \
      ctest --test-dir "$build" -R '^omaroll_tests$' --output-on-failure || test_status=$?
    shopt -s nullglob
    reports=("$build"/tsan-report.*)
    triage_status=0
    if ((${#reports[@]})); then
      python3 tests/tsan-triage.py "${reports[@]}" || triage_status=$?
    else
      echo "tsan-triage: no reports"
    fi
    exit $((test_status || triage_status))
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
