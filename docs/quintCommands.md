# Quint commands for spec/stats.qnt

## Commands used in the workflow
The commands used in [Claude-Quint-C++.md](../../Claude-Quint-C++.md) (flags
checked against `quint --help`, Quint 0.33). Other commands are listed at the
end, in "Other quint parameters".

| Command | What it does | When to use it |
|---|---|---|
| `quint typecheck <file>` | Checks syntax, types and effects (e.g. that actions assign every variable) | After every change to the spec (workflow Phase 3, step 1) |
| `quint run <file> --main=<module> --invariant=<inv>` | Simulator: generates random traces (`init`, then `step` repeatedly) and checks the named boolean `<inv>` in every visited state; stops and prints the trace if it is `false` (violation) | Everyday check of invariants and finding witnesses; fast, no Java, but only sampled traces (Phase 3, step 2) |
| `quint verify <file> --main=<module> --invariant=<inv>` | Exhaustive model checking of **all** reachable states. Backend `apalache` (default; SMT solver, all executions up to `--max-steps`, also temporal formulas) or `tlc` (classic TLA+ model checker; the workflow uses `--backend=tlc`; ignores `--max-steps`). `--step=<action>` selects the step action (default `step`); for this spec the standard step does not finish, so tool-specific steps are used (see "5. Full verification") | Before writing/accepting the implementation; the workflow does not continue until it reports no violation (Phase 3, step 3); slower, needs Java |
| `quint test <file> --main=<module>` | Runs the `run` definitions of the main module whose name ends with `Test` (default filter; other `run`s are skipped). Each test executes its `.then(...)` steps and fails if an `.expect(...)` is `false` or an action cannot execute.<br><br>Options:<br>`--match=<regex>` (select tests, e.g. `--match=flushRetriedTest`)<br>`--max-samples=<n>` (repetitions of randomized tests; 10000 by default, 1 with `--seed`)<br>`--verbosity=3` (show states)<br>`--out-itf=out_{test}_{seq}.itf.json` (save each test's trace) | To check concrete scenarios, like unit tests (Phase 3, step 4) |
| `quint run <file> --main=<module> --mbt --n-traces=<k> --out-itf=<file>` | Without `--invariant` nothing is checked: only generates `k` random ITF/JSON traces (`out0…out<k-1>.itf.json`); `--mbt` adds which action was taken and with which arguments | To produce the traces replayed against the C++ implementation (Phase 5) |

`--main=<module>` selects the module to run. Without it Quint looks for a
module with the same name as the file (`stats` for `stats.qnt`), which does
not exist here (error QNT405). For this spec always use
`--main=StatsFrameworkVerification`. `typecheck` checks all modules and does
not need it.

Other useful options: `--seed` (reproduce a run), `--verbosity` (0–5),
`--max-steps`, `--max-samples` (`run`).

`<inv>` is the name of a boolean `val` from the spec (e.g. `allProperties`,
`inv03Conservation`) or an expression over such names; it is checked in every
visited state. Witnesses (e.g. `w01HolderFull`) are passed to
`--witnesses` instead. Details and examples: see
"How to use `--invariant` and `--witnesses`" near the end of this document.

⚠ To get the trace of a witness, pass its negation to `--invariant`
(e.g. `--invariant='not(w01HolderFull)'`); then "Invariant violated"
means **success**: the scenario was reached. See "Witness traces: 'fail' means
success" at the end.

## Commands for this spec

**Everything at once:** `scripts/check_all.sh` runs steps 1–6 below plus the
C++ build, unit tests and MBT, and fails at the first failing check
(`--quick` skips the slow step 5, full verification). See README.md.

Run all commands below from the spec directory:

```bash
cd stats_framework/spec
```

`--main` must point to `StatsFrameworkVerification`. The `StatsFramework`
module has unset constants and cannot be run on its own.

Recommended order: from the cheapest check to the most expensive one; the spec
is verified before traces for MBT are generated.

### 1. Syntax and type check
Cheapest check; run after every change.
```bash
quint typecheck stats.qnt
```

### 2. Scenario tests (`run ...Test`)
Fast, concrete scenarios with a clear error message (which test, which
`expect` failed).
```bash
quint test stats.qnt --main=StatsFrameworkVerification
quint test stats.qnt --main=StatsFrameworkVerification --match=flushRetriedTest   # a single test
quint test stats.qnt --main=StatsFrameworkVerification --verbosity=3              # show the steps
```

### 3. Simulation with invariants (INV-01…INV-08 and TP-01)
Random traces. Expected result: no violation.
```bash
quint run stats.qnt --main=StatsFrameworkVerification \
  --invariant=allProperties --max-steps=30 --max-samples=10000
```
A single invariant: use its name, e.g. `--invariant=inv03Conservation`.

### 4. Witnesses (and coverage)
Confirm that the important scenarios are reachable at all, and how often. Run
them before full verification: "no violation" means little if these
situations never happen. Invariants are checked in the same run.
```bash
quint run stats.qnt --main=StatsFrameworkVerification --invariant=allProperties \
  --max-steps=20 --max-samples=10000 \
  --witnesses w01HolderFull w02FlushRetried w03InvalidDiscarded \
              w04StopWithRepositoryDown w05FullCycle \
              w06StopDrainsToRepository w07FlusherFull
```
Expected result: "No violation found", and for every witness a line
`... was witnessed in N trace(s) out of 10000 explored (x%)` with N > 0. A
witness near 0% is effectively untested.

Trace of one witness (how it was reached): pass its negation to `--invariant`.
Expected result: a violation; the printed trace is the witness.
```bash
quint run stats.qnt --main=StatsFrameworkVerification \
  --invariant='not(w07FlusherFull)' --max-steps=30 --mbt
```

### 5. Full verification
The gate from the workflow: do not continue until it reports no violation.
Requires Java. Checks all states (or all paths), not only randomly sampled
ones.

**Standard commands (as in the workflow document) — they should work, but do
not finish for this spec in practical time:**
```bash
# Apalache (default backend): all paths up to the given length
quint verify stats.qnt --main=StatsFrameworkVerification --invariant=allProperties --max-steps=10

# TLC backend
quint verify stats.qnt --main=StatsFrameworkVerification --invariant=allProperties --backend=tlc
```
Measured with the default `step` and the earlier model values (holder 3,
flusher 4):
- **Apalache, `--max-steps=10`:** step 3 ~34 s, step 4 ~4 min, step 5 > 2.5 h;
  stopped after ~3 h at step 6 of 10. The number of paths grows exponentially
  with their length.
- **TLC:** never terminates. The counters (`nextSeq`, `dropped`, …) and
  `stored` grow without bound, so the state space is infinite; after ~1 h:
  depth 11, ~82 million distinct states, queue still growing. TLC **ignores
  `--max-steps`**, and Quint passes no state bound to TLC (no `CONSTRAINT`,
  `VIEW`, `SYMMETRY` or depth option; `--tlc-config` only sets heap, stack and
  workers).

**What is used instead: a separate step for each tool.** The module
`StatsFrameworkVerification` defines `stepTlc` and `stepApalache`. They call
the same spec actions as `step`, but choose the inputs each tool can handle
(see "Verification Only" in statsDESIGN.md and conclusions.md). The step is
selected with `--step`:
```bash
# TLC: all distinct states within the bounds (≤ 4 accepted samples, ≤ 1 dropped,
# 1 procedure, durations {5, 11}), at any depth
quint verify stats.qnt --main=StatsFrameworkVerification --backend=tlc \
  --step=stepTlc --invariant=allProperties

# Apalache: all paths up to N steps, with ANY integer duration
quint verify stats.qnt --main=StatsFrameworkVerification \
  --step=stepApalache --invariant=allProperties --max-steps=5
```
Measured (model values holder 2, flusher 3):
- **TLC with `stepTlc`:** ~25 s, 1,401,775 distinct states (13.6 million
  generated), complete state graph of depth 29–30 (TLC with several workers
  reports the depth slightly differently between runs), no violation.
- **Apalache with `stepApalache`:** `--max-steps=4` ~76 s, `--max-steps=5`
  ~5 min, no violation.

### 6. Generate traces for MBT (Phase 5 of the workflow)
Only from the verified spec. Run the script (from any directory):
```bash
./scripts/gen_traces.sh            # writes to traces/
./scripts/gen_traces.sh <dir>      # writes to another directory
```
⚠ Quint quirk: with `--n-traces`, Quint inserts the trace number before the
**first dot of the whole `--out-itf` path**, not of the file name. A dot in
any directory name (e.g. `/tmp/tmp.Ab12/…`) breaks the output path ("no such
file or directory"). The script refuses such paths.
It produces:
- `traces/random/trace0.itf.json … trace499.itf.json`: 500 random traces ×
  30 steps, generated with `--invariant=allProperties` (the run fails if any
  trace breaks an invariant);
- `traces/witness/wNN<Name>.itf.json`: one targeted trace per witness W-01 …
  W-07 (`--invariant='not(wNN...)'`, up to 10,000 attempts). The script fails
  if a witness is not reached. These files have `"status": "violation"` in
  `#meta`, which only means "scenario reached", not an error.

Details of the script:
- **Fixed seed** (`--seed`), so the same spec always gives the same traces.
  With `--seed`, Quint sets `--max-samples` to 1 by default, so the script
  sets it explicitly (500 for random traces, 10,000 for witnesses).
- **Normalized metadata:** Quint writes the creation time and the absolute path
  of the spec into `#meta`; the script removes the time and makes the path
  relative (`jq`). Regenerating from an unchanged spec gives byte-identical
  files on any machine.
- **Traces are test cases and are committed** (~25 MB). Run the script again
  after every change to the spec and commit the result.

Measured: ~20 s for all 507 files.

Single command for the random traces, without the script:
```bash
quint run stats.qnt --main=StatsFrameworkVerification --invariant=allProperties --mbt \
  --max-steps=30 --max-samples=500 --n-traces=500 --seed=0x5747a75 \
  --out-itf=../traces/random/trace.itf.json
```

Note: all commands in steps 1–6 have been run.

### Helpers (not steps)
Used when analysing a failure or preparing a single MBT case.

Save a single trace to a file (ITF/JSON):
```bash
quint run stats.qnt --main=StatsFrameworkVerification --invariant='not(w02FlushRetried)' \
  --max-steps=30 --mbt --out-itf=w02FlushRetried.itf.json
```
`--mbt` adds to the trace which action was taken and with which arguments.

See how a witness was reached. With `--mbt` every state also shows
`mbt::actionTaken` (the action) and `mbt::nondetPicks` (the chosen
arguments):
```bash
# full trace
quint run stats.qnt --main=StatsFrameworkVerification \
  --invariant='not(w07FlusherFull)' --max-steps=30 --mbt

# less noise: --hide needs fully qualified variable names
P=StatsFrameworkVerification::StatsFramework
quint run stats.qnt --main=StatsFrameworkVerification \
  --invariant='not(w07FlusherFull)' --max-steps=30 --mbt \
  --hide $P::discarded $P::dropped $P::lost $P::nextSeq \
         $P::ghostFlushFailedWhileRunning $P::ghostFlushedWhileStopping

# compact view: one line per step (action and arguments)
quint run stats.qnt --main=StatsFrameworkVerification \
  --invariant='not(w07FlusherFull)' --max-steps=30 --mbt 2>&1 \
| grep -E '^\[State|actionTaken|nondetPicks' | paste - - - \
| sed -E 's/mbt::(actionTaken|nondetPicks): //g; s/ +/ /g'
```
Look at the trace, not only at "reached": it shows whether the scenario
happened the way it was meant to (see conclusions.md, "A witness proves only
what its condition says").

Reproduce a specific run (every `quint run` prints the seed it used):
```bash
quint run stats.qnt --main=StatsFrameworkVerification --invariant=allProperties --seed=0x...
```

## How to use `--invariant` and `--witnesses` (what to put as `<inv>`)
`<inv>` is the **name of a boolean `val`** defined in the spec (in the module
given by `--main` or imported into it), or a boolean **expression** over such
names. The simulator evaluates it in every visited state; if it is `false`,
it stops and prints the trace that led there.

Both invariants and witnesses are just boolean conditions; the flag decides
how they are judged:

| Flag | What Quint does with the condition | "Pass" |
|---|---|---|
| `--invariant=<cond>` | checks it in **every** state of every trace | the condition is **always true**; if it is `false` even once, Quint reports a violation |
| `--witnesses <cond> ...` | checks whether the condition was true **at least once**, and counts in how many traces | the condition was true **at least once** (N > 0). Quint does not report an error here; "pass" is our reading of the count |

In short: an invariant is "must always hold" (pass = never broken); a witness
is "must happen" (pass = happened at least once).

Our witnesses `w01…w07` are written in **positive** form (true once the
scenario has happened), so they go to `--witnesses`. Passed directly to
`--invariant` they would be `false` already in the initial state and give a
useless violation. To get the trace of a witness, pass its **negation** to
`--invariant`: `not(w07FlusherFull)` should be "always true" (the scenario
never happens); when the scenario happens it becomes `false` and Quint reports
a violation, which for us is a success (see "Witness traces: 'fail' means
success" below).

There is no special "invariant" keyword or registry in Quint: an invariant is
just a `val ...: bool`. A "group" of invariants is also just a `val` that
combines others with `and`. In `spec/stats.qnt`:

```quint
// StatsFramework – individual invariants
val inv01BoundedCollected: bool = collected.length() <= HOLDER_CAPACITY
val inv03Conservation: bool = ...
// ...
// group: INV-01 … INV-08
val safetyInvariant: bool = and { inv01BoundedCollected, inv02BoundedProcessing, ... }
val tp01StopTerminates: bool = ...

// StatsFrameworkVerification – group: everything checked on every state
val allProperties: bool = safetyInvariant and tp01StopTerminates
// witnesses (scenario in positive form)
val w01HolderFull: bool = MODEL_COLLECTORS.exists(c => dropped.get(c) > 0)
```

⚠ A group contains exactly what is written in its definition; Quint does not
collect invariants automatically. A new invariant (e.g. `inv09…`) must also be
added to `safetyInvariant`, otherwise `--invariant=allProperties` silently
does not check it. (Witnesses cannot be grouped at all; see conclusions.md,
3.5.)

Names you can pass as `<inv>` (or, for witnesses, to `--witnesses`) in this
spec:

| `<inv>` | What is checked | Expected result |
|---|---|---|
| `allProperties` | INV-01…INV-08 and TP-01 | no violation |
| `safetyInvariant` | INV-01…INV-08 | no violation |
| `inv01BoundedCollected` … `inv08NonNegativeCounters` | one invariant | no violation |
| `tp01StopTerminates` | TP-01 | no violation |
| `--witnesses w01HolderFull` … `w07FlusherFull` | one witness (scenario in positive form): in how many traces it happened | N > 0 for every witness |
| `--invariant='not(w01HolderFull)'` … | trace of one witness | **violation** (the trace is the witness) |

Examples (run from `stats_framework/spec`):
```bash
# a group
quint run stats.qnt --main=StatsFrameworkVerification --invariant=allProperties --max-steps=30

# a single invariant
quint run stats.qnt --main=StatsFrameworkVerification --invariant=inv03Conservation --max-steps=30

# an expression combining names (quote it for the shell)
quint run stats.qnt --main=StatsFrameworkVerification \
  --invariant='inv01BoundedCollected and inv03Conservation' --max-steps=30

# several names, reported separately (space-separated, no "=")
quint run stats.qnt --main=StatsFrameworkVerification \
  --invariants inv01BoundedCollected inv03Conservation tp01StopTerminates --max-steps=30
```

Notes:
- A comma-separated list (`--invariant=a,b`) does **not** work in `quint run`
  (error QNT501); use an expression with `and`, or `--invariants a b`.
- If `--invariant` is omitted, it defaults to `true`, i.e. nothing is checked
  (as in the trace-generation command of Phase 5).
- `--witnesses a b` counts in how many traces each named `val` (or
  expression) becomes **true**. It needs the scenario in **positive** form,
  as our `w01…w07` are. A negated condition would be true already in the
  initial state and always report a meaningless 100%.

### ⚠ Witness traces: "fail" means success
To see **how** a witness was reached, pass its **negation** to `--invariant`
(e.g. `--invariant='not(w07FlusherFull)'`). Then a `[violation] Found an issue`
/ `error: Invariant violated` (non-zero exit code) means the scenario **was
reached**; the printed trace shows how. `[ok] No violation found` means it was
**not** reached (wrong condition or too few traces). This applies only to
negated witnesses; for real invariants (`inv…`, `safetyInvariant`,
`allProperties`) a violation is a real bug.

| Passed to `--invariant` | "No violation found" | "Invariant violated" |
|---|---|---|
| invariant (`allProperties`, `inv…`) | ✅ correct | ❌ bug in the spec |
| negated witness (`not(w01HolderFull)` …) | ❌ scenario not reached | ✅ scenario reached |

To avoid confusing the two, keep them apart:
- **invariants** via `--invariant`, expected result "No violation found";
- **witnesses** via `--witnesses` (`w01…w07` are already in **positive**
  form). The run stays green and reports how many traces reached each
  scenario (N > 0 = success), which also shows coverage:
  ```bash
  quint run stats.qnt --main=StatsFrameworkVerification --invariant=allProperties \
    --max-steps=30 --max-samples=10000 \
    --witnesses w01HolderFull w07FlusherFull
  ```
- **witness traces** only when needed, via `--invariant='not(wNN...)'`.

## Other quint parameters
Quint commands not used in the workflow document, but potentially useful.

| Command | What it does | When to use it |
|---|---|---|
| `quint parse <file>` | Checks syntax only (no types) | Rarely; `typecheck` already includes it |
| `quint repl` / `quint -r <file>::<module>` | Interactive Read-Evaluate-Print-Loop: evaluate expressions and execute actions by hand | Exploring and debugging the spec step by step |
| `quint compile <file>` | Translates the spec to another format (e.g. TLA+ or JSON), written to stdout | Integration with other tools (e.g. TLA+ tooling) |
| `quint docs <file>` | Generates documentation from `///` docstrings | Producing reference docs of the spec |

REPL for this spec (not run yet):
```bash
quint -r stats.qnt::StatsFrameworkVerification
# then e.g.: init  →  doStart  →  doStoreStatsRecord("c1","p1",5)  →  collected
```
