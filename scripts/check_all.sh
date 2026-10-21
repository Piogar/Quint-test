#!/usr/bin/env bash
# Runs every check of the project, in the recommended order (cheapest first).
# Fails (exit code 1) at the first failing check.
#
#   1. Quint typecheck
#   2. Quint scenario tests
#   3. Simulation: invariants + every witness reached (coverage printed)
#   4. Full verification: TLC (stepTlc) and Apalache (stepApalache)
#   5. Traces up to date: regenerate into a temp dir and compare with traces/
#   6. C++: configure, build, unit tests and MBT (all traces)
#
# Usage: scripts/check_all.sh [--quick]
#   --quick  skips step 4 (TLC ~30 s, Apalache 4 steps ~80 s)
set -uo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
APALACHE_STEPS=4

fail() { echo "FAIL: $*"; exit 1; }
step() { echo; echo "== $*"; }

step "1. Quint typecheck"
quint typecheck "$SPEC" || fail "typecheck"

step "2. Quint scenario tests"
out=$(quint test "$SPEC" --main=$MAIN 2>&1) || { echo "$out"; fail "quint test"; }
echo "$out" | grep -E 'passing|failing'

step "3. Simulation: invariants and witnesses"
out=$(quint run "$SPEC" --main=$MAIN --invariant=allProperties \
        --max-steps=20 --max-samples=10000 --witnesses $WITNESSES 2>&1) \
  || { echo "$out" | tail -n 20; fail "invariant violated in simulation"; }
echo "$out" | grep -E 'witnessed'
for w in $WITNESSES; do
  echo "$out" | grep -qE "^$w was witnessed in [1-9][0-9]* trace" || fail "witness $w not reached"
done

if [ $QUICK -eq 0 ]; then
  step "4a. Full verification: TLC (stepTlc)"
  out=$(quint verify "$SPEC" --main=$MAIN --backend=tlc --step=stepTlc \
          --invariant=allProperties --verbosity=3 2>&1) || { echo "$out" | tail -n 20; fail "TLC"; }
  echo "$out" | grep -E 'distinct states found|depth of the complete|^\[ok\]'

  step "4b. Full verification: Apalache (stepApalache, $APALACHE_STEPS steps)"
  quint verify "$SPEC" --main=$MAIN --step=stepApalache --invariant=allProperties \
    --max-steps=$APALACHE_STEPS 2>&1 | grep -E '^\[(ok|violation|error|failure)\]'
  [ "${PIPESTATUS[0]}" -eq 0 ] || fail "Apalache"
else
  step "4. Full verification: skipped (--quick)"
fi

step "5. Traces up to date"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/stats_traces_XXXXXX")  # no dot in the path (see gen_traces.sh)
"$SCRIPT_DIR/gen_traces.sh" "$tmp" > /dev/null || { rm -rf "$tmp"; fail "trace generation"; }
if diff -rq "$tmp/random" "$ROOT/traces/random" > /dev/null && diff -rq "$tmp/witness" "$ROOT/traces/witness" > /dev/null; then
  echo "traces/ matches the spec"
  rm -rf "$tmp"
else
  rm -rf "$tmp"
  fail "traces/ is out of date: run scripts/gen_traces.sh and commit the result"
fi

step "6. C++: build, unit tests, MBT"
cmake -S "$ROOT" -B "$ROOT/build" > /dev/null || fail "cmake configure"
cmake --build "$ROOT/build" -j > /dev/null || fail "build"
for t in stats_unit_tests stats_mbt_tests; do
  out=$("$ROOT/build/$t" --gtest_brief=1 2>&1) || { echo "$out"; fail "$t"; }
  echo "$t: $(echo "$out" | grep -E 'PASSED' | tail -n 1)"
done

echo
echo "ALL CHECKS PASSED"
