#!/usr/bin/env bash
# Disposable Arch container only. Desktop hosts use tests/run-isolated.sh.
set -euo pipefail
mode=${1:?fuzz, tsan or coverage}
build=build/$mode
case "$mode" in
  fuzz)
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DOMAROLL_FUZZ=ON
    cmake --build "$build" --parallel 4 --target fuzz_launch fuzz_ipc fuzz_matroska fuzz_pdf fuzz_image fuzz_raw fuzz_subtitles fuzz_organization \
      fuzz_query fuzz_storyboard fuzz_doctext fuzz_exifdate fuzz_hyprland
    bash tests/fuzz/run.sh "$build" "${2:-45}" "${3:-}"
    ;;
  tsan)
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      '-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer' -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
    cmake --build "$build" --parallel 4 --target omaroll_tests omaroll_search_tests omaroll_storyboard_tests
    # Some core tests launch the omaroll executable. Give them an uninstrumented
    # one: this job checks the core suites, not the whole GUI app under TSan.
    cmake -S . -B build/tsan-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    cmake --build build/tsan-app --parallel 4 --target omaroll
    cp build/tsan-app/omaroll "$build/omaroll"
    # Arch's Qt is not built with TSan. Qt's mutexes and thread joins run on
    # futexes inside libQt6Core, where TSan cannot see them, so a run of the
    # whole core suite reports dozens of Qt-internal handoffs (posted events,
    # QFutureInterface, QImage, the environment) and can abort when a thread id
    # is reused. Run the tests whose threading is ours and visible to TSan:
    # the single-instance handshake, the clipboard fallback's std::mutex
    # handoff, sidecar subtitle parsing, organization snapshots and the launch
    # check on their worker, the metadata probe pool, the OCR and similarity
    # indexers, PDF text extraction and its disk-backed matching, and storyboard
    # extraction with its cache lane.
    # Every report then fails the job unless tests/tsan-triage.py knows it as a
    # Qt handoff. Thread leaks are off: Qt never joins its threads where TSan
    # can see it, so every pooled thread would be reported.
    # (ignore_noninstrumented_modules is not usable: it also hides Qt's own
    # thread starts, and TSan then aborts when a thread id is reused.)
    core='^(singleInstance|executableForwards|nativeClipboardFallback|externalSubtitles|subtitle|automaticOrganization|restoringABackup|resumeMigration|repairingDamagedSettings|organizationSnapshots|todaysSnapshot|launchWarns|launchDoesNot|keepingIncomplete'
    core+='|metadataProbes|metadataProgress|indexShutdown|imageDatesUseBoundedBatches'
    core+='|localOcrSearch|sparseOcrRetry|clearingOcrSearch|backgroundTextIndex'
    core+='|exactDuplicateReview|visuallySimilar|similarPictures|similarSets|looseSimilarity|similarityAndDuplicateHashes)'
    search='^(pdfText|pdfReads|pdfIndexing|documentText|textProducers|rowsThatMove|snippetsOfLong|longTextSnippets)'
    # Storyboard tests are threaded throughout; the real-video ones run ffmpeg.
    storyboard='.'
    rm -f "$build"/tsan-report.* "$build"/tsan-tests*.txt
    test_status=0
    # One process per test. TSan's thread registry aborts once Qt reuses a
    # thread id it never saw joined, and a long process with many pooled
    # threads gets there; a short one does not.
    run_suite() {
      local binary=$1 class=$2 pattern=$3 tests test results status=0
      mapfile -t tests < <(QT_QPA_PLATFORM=offscreen "$build/$binary" -functions | sed 's/()$//' |
        grep -E "$pattern" | grep -Ev '^(init|cleanup|initTestCase|cleanupTestCase)$')
      echo "tsan: ${#tests[@]} $binary tests"
      for test in "${tests[@]}"; do
        results="$PWD/$build/tsan-tests-$binary-$test.txt"
        (cd "$build" && QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software PULSE_SERVER=unix:/nonexistent \
          PIPEWIRE_REMOTE=omaroll-no-audio \
          TSAN_OPTIONS="halt_on_error=0:exitcode=0:report_thread_leaks=0:second_deadlock_stack=1:log_path=$PWD/tsan-report:suppressions=$OLDPWD/tests/tsan.supp" \
          "./$binary" -o "$results,txt" "$test" >/dev/null 2>&1) || true
        # exitcode=0 also covers TSan's own aborts, so check that the test ran.
        if ! grep -Eq '^Totals: [0-9]+ passed, 0 failed' "$results" 2>/dev/null ||
           ! grep -Eq "^(PASS|SKIP) +: $class::$test\(" "$results"; then
          echo "tsan: $binary $test did not pass"
          [[ -f $results ]] && grep -E '^(FAIL|QFATAL)' "$results" | head -5
          status=1
        fi
        [[ -f $results ]] && cat "$results" >> "$build/tsan-tests.txt"
      done
      return "$status"
    }
    run_suite omaroll_tests OmarollTest "$core" || test_status=1
    run_suite omaroll_search_tests SearchTest "$search" || test_status=1
    run_suite omaroll_storyboard_tests StoryboardTest "$storyboard" || test_status=1
    shopt -s nullglob
    reports=("$build"/tsan-report.*)
    python3 tests/tsan-triage.py --self-test
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
    if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
      { echo '### Coverage (no threshold)'; echo; echo '```'
        head -n 2 "$build/summary.txt"; grep '^TOTAL' "$build/summary.txt"; echo '```'; } >> "$GITHUB_STEP_SUMMARY"
    fi
    # Coverage is a report, not a gate: the build-and-test jobs gate the
    # suites, and timing-sensitive tests are slower under instrumentation.
    if ((test_status != 0)); then
      echo "::warning::Some tests failed in the coverage build (ctest exit $test_status); see the log above."
    fi
    ;;
  *) exit 2 ;;
esac
