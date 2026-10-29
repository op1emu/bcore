#!/usr/bin/env bash
# The emulator suites CI runs, by name, on build/emu and build/test_linked/:
#   .github/run-suites.sh SUITE...
# runs each in turn, every one even after another fails, and fails if any
# did. Each suite's command is here alone: a job lists the suites it runs,
# so moving a suite from job to job (to balance them) changes one line.
# Every suite skips the tests in .github/emu-ignore.txt; a warning
# (non-failing) is printed for any ignored test that unexpectedly passes,
# prompting cleanup of the list. bash 3.2 (macOS).
set -u
cd "$(dirname "$0")/.."

ignore=(--ignore-list .github/emu-ignore.txt)
cpus=$(getconf _NPROCESSORS_ONLN)
summary=${GITHUB_STEP_SUMMARY:-/dev/null}

now_ms() { python3 -c 'import time; print(time.monotonic_ns() // 1000000)'; }

# bench.s.elf must exit 0 (CHECKREG assertions pass); its wall-clock time
# goes to the job summary.
benchmark() {
  local start end rc=0
  start=$(now_ms)
  ./build/emu build/test_linked/bench.s.elf || rc=$?
  end=$(now_ms)
  echo "Benchmark elapsed: $((end - start)) ms"
  {
    echo "### Benchmark Result"
    echo ""
    echo "| Test | Wall Time | Status |"
    echo "|------|-----------|--------|"
    if [ $rc -eq 0 ]; then
      echo "| bench.s | $((end - start)) ms | PASS |"
    else
      echo "| bench.s | $((end - start)) ms | FAIL (exit $rc) |"
    fi
    echo ""
  } >> "$summary"
  return $rc
}

suite() {
  case $1 in
    O0)
      python3 tests/run_emu_test.py --no-fail-fast "${ignore[@]}" ;;
    # The same suite through the O1 IR pass pipeline, the level op1emu runs.
    # -O 0 never invokes the pipeline, so only this catches IR that the
    # passes miscompile (e.g. poison the backend alone tolerated).
    # Compile-heavy tests take ~15 s at O1, hence the longer timeout.
    O1)
      python3 tests/run_emu_test.py --no-fail-fast -O 1 -t 60 "${ignore[@]}" ;;
    # O1 with every whole RAM page in Memory::page_table(), so loads and
    # stores take the JIT's inline page-table path (op1emu's) and only
    # partial pages, MMRs and faults call FlatMemory.
    O1-page-table)
      python3 tests/run_emu_test.py --no-fail-fast -O 1 -t 60 --page-table "${ignore[@]}" ;;
    # The same with chaining (op1emu's mode): blocks run the next block
    # themselves, through native loops and the chain table.
    O1-page-table-chain)
      python3 tests/run_emu_test.py --no-fail-fast -O 1 -t 60 --page-table --chain "${ignore[@]}" ;;
    # Tiered compilation (promotions on the worker) against untiered, and
    # (diff-copy-patch) the copy-and-patch tier 0 against LLVM's: every
    # test's exit code, output, packets and returns the same (one binary,
    # two sets of flags). A few configurations of each matrix: the whole
    # matrices take about 40 CPU-minutes.
    diff-tiers)
      python3 tests/run_diff_test.py --matrix tiers --only O1-t2-bg5 --count-packets \
        -j "$cpus" -t 60 "${ignore[@]}" ;;
    # op1emu's tiering: size-scaled thresholds, and the light tier-1
    # pipeline over copy-and-patch tier 0.
    diff-tiers-ref-light)
      python3 tests/run_diff_test.py --matrix tiers --only O1-cp-t2-ref2,O1-cp-t1-light --count-packets \
        -j "$cpus" -t 60 "${ignore[@]}" ;;
    diff-copy-patch)
      python3 tests/run_diff_test.py --matrix copy-patch --only tier0 --count-packets \
        -j "$cpus" -t 60 "${ignore[@]}" ;;
    # Promotions on the worker over copy-and-patch tier 0.
    diff-copy-patch-t2-bg1)
      python3 tests/run_diff_test.py --matrix copy-patch --only t2-bg1 --count-packets \
        -j "$cpus" -t 60 "${ignore[@]}" ;;
    # CpuState::packets against single-stepping (--max-steps 1), where
    # every run enters one packet. Long tests time out single-stepped and
    # are reported as skipped.
    packet-count)
      python3 tests/run_packet_count_test.py -t 60 "${ignore[@]}" ;;
    # The test scripts' own tests.
    script-tests)
      python3 -m unittest tests/test_run_diff.py tests/test_capped_run.py ;;
    # build/disasm against bfin-elf-objdump for every assembled object file
    # (build/test_objects/; the Blackfin toolchain on PATH).
    disasm)
      python3 tests/run_comparison_test.py --no-fail-fast ;;
    benchmark)
      benchmark ;;
    *)
      echo "no suite $1" >&2
      return 2 ;;
  esac
}

# Each suite under a banner, not in a collapsed group: a failure shows
# where it is. The table of results goes to the summary at the end, after
# what the suites themselves put there (the benchmark's).
failed=""
rows=""
for name in "$@"; do
  echo "=== $name ==="
  start=$SECONDS
  if suite "$name"; then
    result=pass
  else
    result=FAIL
    failed="$failed $name"
    echo "::error title=$name failed::the suite $name failed (its output follows === $name ===)"
  fi
  echo "=== $name: $result ($((SECONDS - start)) s) ==="
  rows="$rows| $name | $result | $((SECONDS - start)) s |
"
done
{
  echo "| Suite | Result | Time |"
  echo "|-------|--------|------|"
  printf %s "$rows"
} >> "$summary"

if [ -n "$failed" ]; then
  echo "failed:$failed"
  exit 1
fi
