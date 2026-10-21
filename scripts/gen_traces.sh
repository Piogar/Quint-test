#!/usr/bin/env bash
# Generate ITF traces from spec/stats.qnt for the C++ MBT harness
# (Phase 5 of the workflow).
#
#   <out>/random/   500 random traces × 30 steps (fixed seed, reproducible)
#   <out>/witness/  one targeted trace per witness W-01 … W-07
#
# Usage: scripts/gen_traces.sh [output-dir]   (default: traces/)
#
# Traces are test cases and are committed to the repository. Regenerate them
# (run this script again) after every change to the spec.
set -uo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
OUT="$(mkdir -p "${1:-$ROOT/traces}" && cd "${1:-$ROOT/traces}" && pwd)"

# Quint quirk: with --n-traces it inserts the trace number before the FIRST
# dot of the whole --out-itf path (not of the file name), so a dot in any
# directory name breaks the output path.
case "$OUT" in
  *.*) echo "FAIL: output path must not contain '.' (Quint --n-traces quirk): $OUT"; exit 1 ;;
esac

SEED=0x5747a75        # fixed seed: the same spec always gives the same traces
N_TRACES=500
MAX_STEPS=30

rm -rf "$OUT/random" "$OUT/witness"
mkdir -p "$OUT/random" "$OUT/witness"

echo "== random traces ($N_TRACES × $MAX_STEPS steps)"
# --invariant=allProperties: no generated trace passes through a state that
# breaks the invariants (the run fails if one does)
# (with --seed, --max-samples defaults to 1, so it is set explicitly)
quint run "$SPEC" --main=$MAIN --invariant=allProperties --mbt \
  --max-steps=$MAX_STEPS --max-samples=$N_TRACES --n-traces=$N_TRACES --seed=$SEED \
  --out-itf="$OUT/random/trace.itf.json" > "$OUT/random.log" 2>&1 \
  || { echo "FAIL: quint run failed (see $OUT/random.log)"; exit 1; }
rm -f "$OUT/random.log"
echo "   $(ls "$OUT/random" | wc -l) files in random/"

echo "== witness traces"
fail=0
for w in $WITNESSES; do
  # The negated witness as invariant: a violation means the scenario was
  # reached, and the violating trace is written to the file.
  # (up to 10,000 attempts; with --seed the default would be 1)
  if quint run "$SPEC" --main=$MAIN --invariant="not($w)" --mbt \
       --max-steps=$MAX_STEPS --max-samples=10000 --seed=$SEED \
       --out-itf="$OUT/witness/$w.itf.json" > /dev/null 2>&1; then
    echo "   FAIL: $w not reached"; fail=1
    rm -f "$OUT/witness/$w.itf.json"
  else
    echo "   $w -> witness/$w.itf.json"
  fi
done

# Quint writes the creation time (timestamp, description) and the absolute
# path of the spec (source) into "#meta". Remove the time and make the path
# relative, so that regenerating from an unchanged spec gives byte-identical
# files on any machine and the repository shows no spurious changes.
for f in "$OUT"/random/*.itf.json "$OUT"/witness/*.itf.json; do
  jq -c 'del(."#meta".timestamp, ."#meta".description) | ."#meta".source = "spec/stats.qnt"' \
    "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done

[ $fail -eq 0 ] && echo "DONE" || exit 1
