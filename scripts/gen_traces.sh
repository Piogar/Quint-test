#!/usr/bin/env bash
# Generate ITF traces from spec/stats.qnt for the C++ MBT harness
# (Phase 5 of the workflow).
#
#   traces/random/   500 random traces × 30 steps (fixed seed, reproducible)
#   traces/witness/  one targeted trace per witness W-01 … W-07
#
# Traces are test cases and are committed to the repository. Regenerate them
# (run this script again) after every change to the spec.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC="$ROOT/spec/stats.qnt"
MAIN=StatsFrameworkVerification
OUT="$ROOT/traces"

SEED=0x5747a75        # fixed seed: the same spec always gives the same traces
N_TRACES=500
MAX_STEPS=30
WITNESSES="w01HolderFull w02FlushRetried w03InvalidDiscarded w04StopWithRepositoryDown w05FullCycle w06StopDrainsToRepository w07FlusherFull"

rm -rf "$OUT/random" "$OUT/witness"
mkdir -p "$OUT/random" "$OUT/witness"

echo "== random traces ($N_TRACES × $MAX_STEPS steps)"
# --invariant=allProperties: no generated trace passes through a state that
# breaks the invariants (the run fails if one does)
# (with --seed, --max-samples defaults to 1, so it is set explicitly)
quint run "$SPEC" --main=$MAIN --invariant=allProperties --mbt \
  --max-steps=$MAX_STEPS --max-samples=$N_TRACES --n-traces=$N_TRACES --seed=$SEED \
  --out-itf="$OUT/random/trace.itf.json" > "$OUT/random.log" 2>&1 \
  || { echo "FAIL: quint run failed (see traces/random.log)"; exit 1; }
rm -f "$OUT/random.log"
echo "   $(ls "$OUT/random" | wc -l) files in traces/random/"

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
    echo "   $w -> traces/witness/$w.itf.json"
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
