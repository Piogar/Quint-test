# Conclusions

## 1. General conclusions

### Keep code design and test design separate
It is crucial to keep what the **code** must do and what the **tests** need in
separate, clearly marked parts of the design. Both are generated from the same
document (Quint spec, C++ code, MBT harness), so anything mixed in leaks to the
wrong side.

**Code design (implementation behavior):** Types, Abstract State, Constants,
Flusher Conversion Rules, Operations, Queries, Spec to Implementation Mapping.

**Test design (verification only):** State Invariants, Temporal property,
Behavioral Guarantees, Verification Only (model constants, `lastResult`,
`ghost*` variables, witnesses), the in-memory fake repository and
`SetRepositoryAvailable`, test-access classes for snapshots.

Why it matters:
- **No test artifacts in production.** The mapping rule says anything in the
  spec not listed there is verification-only and must not appear in the
  implementation. That only works if the split is explicit.
- **Tests must not shape the production API.** Examples from this design:
  `StoreResult` existed mainly so MBT could check `StoreStatsRecord` and was
  removed (callers must not react); snapshots and `nextSeq` access are needed
  only by MBT and go through test-only access, not public methods.
- **Independent change.** Adding a witness, flag or invariant must not change
  the product; changing a method signature must not change what is verified.
- **Clear review.** Reviewers can check requirements without verification
  noise, and verification without implementation detail.

### What each kind of property checks
State Invariants and Temporal properties check the **spec** (is the design
correct?); Behavioral Guarantees check the **implementation** (does the code
behave like the spec?).

| | State Invariants (INV) | Temporal properties (TP) | Behavioral Guarantees (BG) |
|---|---|---|---|
| **Question** | Is every state correct? | Will something **eventually** happen? | Does the implementation do the same as the spec? |
| **About** | one, current state | the future after a given state ("if P now, then eventually Q") | a sequence of operations executed on the implementation |
| **What is checked** | the spec (model) | the spec (model) | the **C++ code** against the spec |
| **Tool** | `quint run` / `quint verify` | `quint run` (bounded version, e.g. "within N steps") or `quint verify` (temporal formula) | MBT: traces from the spec replayed against the code |
| **In Quint** | `val invNN...: bool`, a condition on the state | `val tpNN...` as `trigger implies future`, or `temporal ... = always(... eventually ...)`; the trigger (left side of `implies`) defines for which states it applies | not in Quint; implemented by the MBT harness |
| **Can MBT check it?** | indirectly (C++ state = spec state, and the spec satisfies INV) | **no**, a trace is finite | **yes**, that is its job |
| **Typical bug caught** | design bug: an operation leads to a wrong state | design bug: the system can get stuck (e.g. never reach `Stopped`) | code bug: different result, different state, wrong order |
| **Example** | INV-01: `collected` ≤ `HOLDER_CAPACITY` | TP-01: from `Stopping`, Fetch, Flush, Finish reach `Stopped` | BG-02: after every step the C++ state equals the spec state |

How they fit together:
1. **INV and TP** give confidence that the **spec** (design) is right.
2. **BG** give confidence that the **code** is faithful to the spec.
3. Together: the code has the same good properties as the design, so
   invariants need not be re-checked on the implementation; it is enough that
   the implementation matches the spec at every step.

### ⚠ Beware of low coverage: "no violation" is not enough
A green `quint run --invariant=...` only says that **no violation was found in
the traces that were explored**. It says nothing about **which situations those
traces actually reached**. If interesting situations are rare, invariants are
checked mostly on trivial states, and the same weak traces are later replayed
against the C++ code by MBT.

**Measured on this spec** (10,000 random traces × 20 steps, uniform choice
among the 7 operations in `step`; `quint run ... --witnesses <conditions>`):

| Situation | Traces where it happened |
|---|---|
| at least one sample accepted | 52.95% |
| something stored in the repository | 9.26% |
| W-01 HolderFull | 1.45% |
| W-02 FlushRetried | **0.39%** (39 traces) |
| W-03 InvalidDiscarded | 6.46% |
| W-04 StopWithRepositoryDown | 2.18% |
| W-05 FullCycle | 2.81% |
| W-06 StopDrainsToRepository | 3.23% |
| W-07 FlusherFull | **0.17%** (17 traces) |

So ~47% of traces never accept a sample, and the hardest scenarios are
exercised by only a few dozen traces. The example trace printed by `quint run`
can look completely empty (`collected` and `stored` always `[]`).

**Why:** every operation is always enabled (rejections are steps, needed for
BG-01/BG-04), and `step` picks among them uniformly. Many steps are rejected
calls (`Start`, `Stop`, `Finish` in the wrong state), and reaching "holder
full" or "flusher full" needs many `StoreStatsRecord` calls in a row.

**What to do:**
- **Always measure coverage**, not only invariants. Pass each scenario in
  positive form to `--witnesses` (names or expressions); the output gives the
  share of traces in which it happened. A scenario near 0% is effectively
  untested.
- **Witnesses as `--invariant` only prove reachability** (at least one trace),
  not how often a situation is checked.
- **Fix coverage in the verification `step`, not in the spec.** Guarded
  actions in the spec would improve coverage but remove rejections from traces
  (MBT would no longer test that the code rejects correctly). Instead, put
  guards in `step` so that most steps can succeed, and keep a small share of
  unrestricted calls so rejections still appear in traces. Re-measure after
  each change.
- **Use `quint verify`** for an exhaustive check of short executions; it does
  not depend on random sampling.

**Result after improving `step`** (same measurement, 10,000 traces × 20
steps). `step` now has guards (most steps are operations that can succeed),
weights (a branch listed several times is picked proportionally more often;
`StoreStatsRecord` is listed twice) and one `anyCall` branch with
unrestricted calls:

| Situation | Uniform `step` | Guarded + weighted `step` |
|---|---|---|
| at least one sample accepted | 52.95% | **94.16%** |
| something stored in the repository | 9.26% | **37.94%** |
| W-01 HolderFull | 1.45% | 15.31% |
| W-02 FlushRetried | 0.39% | 2.89% |
| W-03 InvalidDiscarded | 6.46% | 29.22% |
| W-04 StopWithRepositoryDown | 2.18% | 27.57% |
| W-05 FullCycle | 2.81% | 26.61% |
| W-06 StopDrainsToRepository | 3.23% | 29.83% |
| W-07 FlusherFull (overflow while repository down) | — | 1.91% |
| any overflow | 0.17% | 3.15% |

Rejected calls still appear: each kind of rejection (`Start`, `Fetch`,
`Flush`, `Stop`, `Finish`, ignored `StoreStatsRecord`) occurs in 41–71% of
traces, so MBT keeps testing rejections. W-02 and W-07 are still the rarest
scenarios; their weight can be raised further if needed.

### ⚠ A witness proves only what its condition says
The simulator looks for the **cheapest** trace that satisfies a witness
condition. If the condition is looser than the scenario's description, it
finds a shortcut and the witness is reported as reached, although the
described scenario was never shown.

Example from this spec: W-07 is described as "processing overflows **during a
repository outage**", but its first condition was only `overflowed[c] > 0`.
The simulator reached it with a working repository: new samples were
collected between two `Fetch` steps and no `Flush` ran in between, so
`processing` still held the earlier samples and overflowed. Running more
traces would not help: the condition simply did not ask for an outage.

When writing a witness:
- **Check that the condition covers every part of the description**
  ("during an outage", "while stopping", "after a failure", …).
- **If the description is about the moment of an event, not the current
  state, use a `ghost` variable.** Counters such as `overflowed` only grow and
  do not show when or under which conditions they grew; a check such as
  `overflowed > 0 and not(repoAvailable)` would mix the moment of the overflow
  with a later state. W-07 now uses `ghostOverflowWhileRepoDown`, set in the
  step where `overflowed` grows while the repository is unavailable.
- **Look at the witness trace**, not only at "reached": it shows whether the
  scenario happened the way it was meant to.

### ⚠ Exhaustive verification is not free, even for a small system
The expectation was that Quint checks **all combinations**, so no corner case
can be forgotten. In practice, even this small, single-threaded model (7
operations; most with 0–1 arguments, one with 3 arguments of 2, 2 and 5
values) could not be verified exhaustively at the depth suggested by the
workflow document.

**Paths vs. states**
- **State:** one valuation of all variables (`state`, `collected`,
  `processing`, `stored`, counters, `lastResult`, `ghost*`).
- **Path (trace):** a sequence of steps from the initial state. Many paths
  lead to the same state.

This model has both problems:

| Problem | Cause in this model | Size |
|---|---|---|
| **Many paths** | 27 different operations per step (`StoreStatsRecord` alone has 2 collectors × 2 procedures × 5 durations = 20 variants), all always possible | 27ᴺ paths of length N (10 steps ≈ 2 × 10¹⁴) |
| **Many (infinitely many) states** | counters (`nextSeq`, `dropped`, …) and `stored` grow without bound; lists of samples with `seq` numbers, `lastResult` and `ghost*` multiply the combinations | unbounded |

**How the tools deal with it**

| | Apalache (`quint verify`, default) | TLC (`quint verify --backend=tlc`) | Simulator (`quint run`) |
|---|---|---|---|
| Method | symbolic, bounded by length: all paths of length ≤ N encoded as one formula for an SMT solver | explicit states: walks the state graph and expands every **distinct** state once | samples random paths |
| Merges identical states? | no; the problem grows with N even if paths converge | yes; paths are never enumerated, only distinct states count | no |
| Bounded by | `--max-steps` (length) | a condition on the state (e.g. counters ≤ 2); **ignores `--max-steps`** | `--max-steps`, `--max-samples` |
| Cost grows with | number of paths: exponential in N | number of distinct states | number of samples |

**Measured** (model values: 2 collectors, 2 procedures, 5 durations,
holder 3, flusher 4; invariant `allProperties`):
- **Apalache, `--max-steps=10`** (command from the workflow document): step 3
  took ~34 s, step 4 ~4 min, step 5 > 2.5 h. **Stopped after ~3 h at step 6
  of 10.**
- **TLC, no state bound:** after ~1 h at depth 11: **~82 million distinct
  states** (~486 million generated), ~3 million new distinct states per
  minute, and the queue of unexplored states still growing (~74.5 million).
  It never terminates, because the state space is infinite.
- **Simulator, 10,000 traces × 30 steps:** ~1–2 s, but only sampled.

So both kinds of explosion appear: Apalache suffers from the number of paths,
TLC from the number of distinct states; and even bounded by depth 11 the number
of distinct states is already tens of millions.

**What could reduce it (not yet measured):**
- smaller domains in the verification instance: the procedure does not affect
  the logic (1 value is enough) and durations need only one valid and one
  invalid value → 11 operations per step instead of 27 (boundaries of
  `isValid` then need a separate small test);
- for TLC: a state bound (e.g. ≤ 2 samples per collector), comparing states
  without `lastResult` and `ghost*` (VIEW), symmetry `c1` ↔ `c2`;
- cheaper invariants (INV-04, INV-05 compare every pair of samples);
- an inductive invariant in Apalache (proof for any length in one step; needs
  manual strengthening).

**What was changed, and the result.** Only the verification module changed;
the spec (`StatsFramework`) and the architecture stayed unbounded.
1. **Smaller model sizes, chosen by a rule:** a size is bounded just above
   every threshold that appears in the logic. Holder 2 (was 3) still reaches
   "holder full"; flusher 3 (was 4) is still larger than the holder, so a
   `Fetch` can move only part of the samples. This also raised simulation
   coverage (W-01 15% → 32%, W-07 2% → 3%).
2. **A separate step per tool** (`quint verify --step=...`), calling the same
   spec actions as the simulation `step`:
   - `stepTlc`: 1 procedure, durations `{5, 11}`, at most 4 accepted samples
     and 1 sample dropped by a full holder. This makes the state space finite.
     Counters can be bounded without losing behavior because the logic never
     branches on their value (only on capacities, which the bounds exceed).
   - `stepApalache`: any integer duration (`Int`); the SMT solver finds the
     values that matter itself (a test showed it picks the boundary value
     `MAX` without being told). Length bounded by `--max-steps`.
   - Both list every operation once; guards and weights of the simulation
     `step` only bias random choice and are useless for exhaustive tools.
3. **A scenario test for the boundaries of `isValid`** (-1, 0, 10, 11),
   because TLC uses only `{5, 11}`.

| Run | Before | After |
|---|---|---|
| TLC | no bound: never terminates (~82 million distinct states after ~1 h) | `stepTlc`: **~25 s**, 1,401,775 distinct states, complete state graph of depth 29, no violation |
| Apalache, 4 steps | ~4 min (step 4 of the `--max-steps=10` run) | `stepApalache`: **~76 s**, no violation |
| Apalache, 5 steps | > 2.5 h for step 5 alone | `stepApalache`: **~5 min**, no violation |
| Apalache, 10 steps | stopped after ~3 h at step 6 | not tried; TLC already covers all bounded states at any depth |

**Values vs. sizes.** The reductions split into two kinds:
- **Values** (what is passed, e.g. the duration): with Apalache they need not
  be chosen by a human at all; a symbolic value (`Int`) lets the solver find
  the significant values from the conditions in the logic. TLC and the
  simulator need finite sets, chosen by equivalence classes.
- **Sizes** (how many: collectors, capacities, samples, steps): must be
  bounded for any finite check. The rule "just above every threshold in the
  logic" makes the choice mechanical; a counter that no condition reads needs
  no headroom at all.

**Costs and limitations of this approach.**
- **Complexity:** three steps (`step`, `stepTlc`, `stepApalache`) instead of
  one, plus TLC-specific constants. The verification module becomes harder to
  read, and each tool's inputs must be kept consistent with the spec. This is
  a real drawback; it was accepted only because the standard command does not
  finish.
- **Quint's TLC integration is limited:** Quint generates the TLC
  configuration with only `INIT`, `NEXT`, `INVARIANT` and `PROPERTY`, passes
  fixed TLC arguments, and `--tlc-config` only sets heap, stack and workers.
  State bounds (`CONSTRAINT`), `VIEW`, `SYMMETRY` and a depth limit (`-dfid`)
  are not available, so the bound had to be written into the model
  (`stepTlc`). Running TLC outside Quint (on the TLA+ produced by
  `quint compile`) would allow them, at the cost of leaving Quint.
- **Residual risk:** a bug that needs more than 4 samples, more than 2
  collectors or larger capacities would not be found by TLC; no condition in
  the logic requires such sizes, which is the argument that the risk is small.

**The role of the human.** All the reductions above require human analysis
(which values are equivalent, which data does not affect the logic). This is
the same analysis as choosing equivalence classes for manual tests. What Quint
still takes over is the harder part: **combinations and orderings of
operations** and the **expected result** of every case (derived from the spec
and the invariants). With few interactions between operations, as in this
single-threaded framework, that gain is moderate and well-written unit /
module tests could cover much of it. The gain grows with the number of
interactions, above all with concurrency (Phase 2), where the combinations are
practically impossible to enumerate by hand.

**Conclusion:** exhaustive verification is not automatic. The model must be
designed for verification (small domains, no data that does not affect the
logic, bounded counters) and the tool chosen for the problem (TLC with a state
bound for "all states", Apalache only for short paths). Otherwise even a small
model explodes. With these choices the same spec went from "does not finish
in hours" to ~25 s (TLC, all bounded states) and ~5 min (Apalache, 5 steps
with any duration). In practice the assurance is layered: exhaustive checks of
short or bounded executions, random simulation with measured coverage for long
ones, and MBT for the implementation.

## 2. Design document (statsDESIGN.md)

### 2.1 Language-neutral part vs. language-specific part
- Near the end of [statsDESIGN.md](statsDESIGN.md) there is the section
  **"Spec to Implementation Mapping (C++)"**. It is directly related to C++
  implementation details: class ownership of data, method names and
  signatures, `StatsConfig`, the `tick()` loop, and test-only elements such as
  the in-memory fake repository.
- All other sections (state, operations, queries, invariants, behavioral
  guarantees) are language-neutral.
- The mapping is used both by code generation (what to produce) and by the MBT
  harness (which method to call per spec step, which accessor to read per
  state variable).
- It contains implementation decisions, not requirements, and is the part most
  likely to change. It could later be moved to a separate file (e.g.
  `docs/statsMapping_cpp.md`) to keep the design document language-neutral.

### 2.2 Verification-only elements
Every verification-only element is listed in the "Verification Only" section,
including the `ghost` naming rule for spec variables that have no counterpart
in the implementation (see 1 for which sections are code design and which are
test design).

### 2.3 Encapsulation vs. test observability
MBT (BG-02 StateConformance) compares the implementation's state with the
spec's after every step, so tests must be able to read that state. Not all of
it should be part of the production API.

#### Accessors that are legitimate public API
Needed in production as well, so they do not break encapsulation: they expose
counters and sizes, not internals.

| Accessor | Production use |
|---|---|
| `StatsOrchestrator::state()` | application checks whether shutdown completed |
| `dropped(c)`, `overflowed(c)`, `discarded(c)`, `lost(c)` | monitoring and alerting |
| `collectedCount()`, `processingCount()` | health metrics (backlog size) |

#### Accessors needed only by tests
| Accessor | Why MBT needs it |
|---|---|
| `StatsHolder::collectedSnapshot()` | compare the contents of `collected` after every step |
| `StatsFlusher::processingSnapshot()` | compare the contents of `processing` after every step |
| `setAvailable(b)` | lives only on the in-memory fake repository, which is test code anyway |

The two snapshots are the issue: production never needs to see the samples
inside the holder or the flusher. Making them public would expose internals to
every class.

#### Ways to overcome it
| Option | How it looks | Pros | Cons |
|---|---|---|---|
| **A. Friend test-access class** | `StatsHolder` and `StatsFlusher` declare `friend class StatsTestAccess;`. `StatsTestAccess` exists only in `tests/` and reads the private data | public API stays clean; the same build for production and tests; cost: one `friend` line per class | production code "knows" the name of a test class (only the name, no code) |
| **B. `protected` members + test subclass** | members `protected`; in tests `class TestableHolder : public StatsHolder` with read methods | no mention of tests in production code | forces `protected` where `private` would be enough; tests must create subclasses instead of the real objects |
| **C. `#ifdef TESTING` accessors** | snapshot methods compiled only in the test build | simple | **the tested build differs from the production build**; avoid |
| **D. No snapshots** | MBT compares only counters, sizes and the repository contents (via the fake) | no extra access at all | weaker: e.g. a wrong order in the holder is detected only after the next `Flush` |

**Decision (for now): A.** Keep `state()`, counts and counters public; give
tests access to the snapshots only through `StatsTestAccess`, declared as a
friend of `StatsHolder` and `StatsFlusher`. `StatsOrchestrator::finish()` is
public (the application may call it; MBT calls it directly), so it needs no
special access.

### 2.4 Prompt for writing the design document
Produces (or reviews) a design document that follows the rules in 1 and 2.1–2.3
and can serve as the single source for the Quint spec, the C++ code and the
MBT harness.

```
Write (or revise) the design document docs/<name>DESIGN.md for <system>.
It is the single source for generating the Quint spec, the C++ code and the
MBT harness, so code design and test design must be kept in separate, clearly
marked sections.

Code design (implementation behavior), language-neutral:
- Overview: components and their responsibilities; data flow; execution model
  (threads, what is atomic).
- Types: records and enums used by the system.
- Abstract State: every state variable with its type, owning component and
  initial value. Only what affects observable behavior.
- Constants: configuration fixed at creation, with valid ranges.
- Rules: pure rules the system applies (e.g. validation, conversion).
- Operations: one subsection per operation with its caller, then
  "Succeeds when / Rejected when" branches, each with an exact Effect on the
  state and a Result. State which operations report a result to the caller
  and which return nothing (fire-and-forget). A rejected branch changes
  nothing unless its Effect says otherwise.
- Queries: read-only values derived from the state, with the owning component.
- No language-specific details (class names, signatures, containers) outside
  the mapping section.

Test design (verification only):
- State Invariants and Temporal properties check the SPEC (is the design
  correct?); Behavioral Guarantees check the IMPLEMENTATION (does the code
  behave like the spec?). Together: the code has the same properties as the
  design, so invariants need not be re-checked on the implementation.
- State Invariants: properties of every single state, IDs INV-01, INV-02, ...
  Checked on the spec.
- Temporal properties: requirements about the future ("after X, eventually
  Y"), IDs TP-01, ... A state alone cannot decide them. State where each is
  checked: on the spec (as a bounded check, e.g. "within N steps", or as a
  temporal formula); MBT cannot check "eventually".
- Behavioral Guarantees: properties over sequences of operations, checked on
  the implementation by MBT (traces from the spec replayed against the code),
  IDs BG-01, ...; include result conformance, state conformance and
  rejection behavior.
- Verification Only: small model values for the constants, finite domains for
  operation arguments (including invalid values), environment actions, and
  every helper that exists only for verification. Spec variables that have no
  counterpart in the implementation are prefixed `ghost` and are not compared
  by MBT.
- Witnesses: scenarios the spec must be able to reach, IDs W-01, ..., each
  with the state condition that identifies it.

Language-specific part, in one separate section
("Spec to Implementation Mapping (<language>)"):
- For every spec element (state variable, constant, type, operation, query)
  the implementation element: class, method, signature.
- How the MBT harness drives each operation and reads each state variable.
- Accessors: mark which are public API (needed in production, e.g. state,
  counters, sizes) and which are needed only by tests (e.g. snapshots of
  internal containers). Test-only access must not be public API; use a
  test-access mechanism (e.g. a friend test-access class).
- Rule: anything in the spec not listed in the mapping is verification-only
  and must not appear in the implementation.

Checks before finishing:
- No result exists only so that tests can check it; callers must not need to
  react to it (otherwise remove it and let tests observe the state).
- Every verification-only element is in the test-design sections or in
  "Verification Only", never in the code-design sections.
- Every INV/TP/BG/W item has an ID and refers only to state, operations and
  results defined above.
```

## 3. Quint specification (spec/stats.qnt)

### 3.1 Logical assumptions
- **Two modules in one file.** `StatsFramework` is the spec: behavior only,
  parameterized by the design-doc constants. `StatsFrameworkVerification`
  holds the small model values, `init`, `step`, witnesses and scenario tests.
  Verification is not split into more modules, because Quint does not
  re-export an imported instance.
- **Rejected operations are steps.** Actions have no guards; a rejected call
  leaves the state unchanged (or applies exactly the effect the doc lists)
  and records the failure in `lastResult`, so MBT traces include rejections
  (BG-01, BG-04).
- **Operations are independent steps in any order.** The spec allows
  orderings that the production orchestrator never produces: `tick()` always
  calls `fetch()` and then `flush()`, but the spec also allows e.g. `Fetch,
  Fetch` without a `Flush` in between (so `processing` can overflow even with
  a working repository). Consequences: invariants are checked on more
  behaviors than production can show, which makes them stronger (and the
  framework stays correct even with a buggy orchestrator); witnesses must be
  precise, otherwise the simulator reaches them through such an ordering (see
  "A witness proves only what its condition says"); the C++ classes and the
  MBT harness must accept these orderings too, because MBT calls `fetch()`,
  `flush()` and `finish()` directly.
- **Logic in pure functions, steps in actions** (see 3.2).
- **`ghost*` variables** exist only for verification (e.g. history flags for
  witnesses that describe a sequence of events). They have no counterpart in
  the implementation and are not compared by MBT.
- **`step` belongs to verification.** It chooses which calls to explore and
  with which arguments (small model domains, including invalid values); the
  spec only defines what each call does.
- **Witnesses are `val`s in positive form** (`wNN<Name>`, true once the
  scenario has happened). They are checked with `--witnesses`: the run stays
  green and reports in how many traces each scenario was reached, which also
  shows coverage. Invariants are checked with `--invariant` in the same run.
  The trace of one witness (how it was reached, e.g. as a targeted MBT case)
  is produced on demand with `--invariant='not(wNN<Name>)'`; there a violation
  means success. Earlier the witnesses were written as negations, which made
  `--invariant` results read the other way round and did not work with
  `--witnesses`. Commands and details: [quintCommands.md](quintCommands.md),
  section "How to use `--invariant` and `--witnesses`".
- **Bounded temporal properties are state properties.** TP-01 is checked in
  every visited state by composing pure functions, not by a temporal formula.
- **No implementation details in the spec.** No C++ class/method names or
  signatures; the mapping stays in the design doc.

### 3.2 Structure: state, transitions, results
In [../spec/stats.qnt](../spec/stats.qnt) every operation is split into logic
(a pure function) and a step (an action):

```quint
type Transition = { sys: Sys, result: OpResult }
pure def mkTr(s: Sys, r: OpResult): Transition = { sys: s, result: r }

pure def fetchOp(s: Sys): Transition = ...          // logic: what happens
action doFetch = applyTransition(fetchOp(current))  // step: write it to the state variables
```

- **`Sys`**: the whole system state in one record (everything from "Abstract
  State": `state`, `collected`, `processing`, `stored`, counters,
  `repoAvailable`). It does not contain `lastResult` or `ghost*` variables:
  those are not framework state.
- **`OpResult`**: what the caller got back from the last operation. A sum type
  (like a Rust `enum` with data, or `std::variant` in C++): each variant names
  the operation and carries its result, e.g. `StopResult(false)` = "Stop was
  called and was rejected". Variants such as `StopResult` are constructors of
  `OpResult`, not separate types.
- **`Transition`**: the outcome of one operation: the **new state** (`sys`) and
  the **result for the caller** (`result`).
- **`mkTr`**: convenience constructor for `Transition`; `mkTr(s, r)` is the
  same as `{ sys: s, result: r }`.
- **`*Op` (pure functions)**: describe what an operation does (success /
  rejection conditions, new state, result). They change nothing, like a C++
  free function `Transition fetchOp(const Sys&)`.
- **`do*` (actions)**: perform a step: call the pure function on `current` and,
  via `applyTransition`, write `sys` into the state variables, `result` into
  `lastResult`, and update the `ghost*` variables.

C++ equivalent:
```cpp
struct Sys        { OrchestratorState state; std::vector<StatRecord> collected; /* ... */ };
struct Transition { Sys sys; OpResult result; };
Transition mkTr(Sys s, OpResult r) { return { s, r }; }
```

**Why pure functions at all:** TP-01 must be checked in every `Stopping` state
by computing "what if Fetch, Flush, Finish ran now" without actually running
them: `finishOp(flushOp(fetchOp(current).sys).sys)`. Quint actions cannot be
run "on a copy", pure functions can. All operations use the same pattern for
consistency and so the state is written in one place (`applyTransition`).
Witnesses and scenario tests do not need pure functions; they only use actions
and read state.

### 3.3 Prompt for generating the Quint spec
A modified version of the original example prompt (see 3.4 for what changed
and why). It produces a spec in the style of [../spec/stats.qnt](../spec/stats.qnt).

```
Generate a Quint specification from docs/statsDESIGN.md into spec/stats.qnt.

Modules (one file, two modules):
- `StatsFramework`: the spec. A `const` for every design-doc Constant; no
  model values here.
- `StatsFrameworkVerification`: imports `StatsFramework` with the small model
  values from "Verification Only"; contains `init`, `step`, witnesses and
  scenario tests. Do not split verification into more modules (Quint does not
  re-export an imported instance).
- Header comment with the naming rule: variables prefixed `ghost` exist only
  for verification, have no counterpart in the implementation and are not
  compared by MBT.

StatsFramework:
- Types from "Types" (records; sum types for enums).
- A typed `var` for each state variable listed in "Abstract State", and a
  record type `Sys` holding all of them.
- `var lastResult: OpResult`, where `OpResult` is a sum type with one variant
  per operation carrying its result (e.g. `StopResult(bool)`,
  `FetchResult({ ok: bool, n: int })`) plus `NoResult` for the initial state.
  For an operation that reports nothing to its caller, use a verification-only
  label variant and say so in a comment.
- `type Transition = { sys: Sys, result: OpResult }` and helper
  `pure def mkTr(s, r)`.
- One pure function `<op>Op(s: Sys, args...): Transition` per operation,
  implementing every "Succeeds when / Rejected when" branch. A rejected branch
  returns `s` unchanged (or with exactly the effect the doc lists) and a
  failure result.
- One action per operation, `do<Op> = applyTransition(<op>Op(current, args))`.
  No guards: a rejected call is still a step and records the failure in
  `lastResult`, so MBT traces include rejections.
- `applyTransition(t)` assigns every state variable from `t.sys` (variables an
  operation does not modify are carried over unchanged in `t.sys`),
  `lastResult` from `t.result`, and every `ghost*` variable via pure helper
  functions.
- An `init` action (`specInit`) setting all vars, including `lastResult` and
  `ghost*`, to their initial values.
- `ghost*` variables for witnesses that describe a sequence of events
  (history flags), updated in `applyTransition`.
- Queries from "Queries" as `val`s.
- A `val` for each individual invariant, named `invNN<Name>`
  (e.g. `inv01BoundedCollected`), with a comment carrying the exact ID
  (`// INV-01 BoundedCollected`).
- A `val safetyInvariant: bool` combining all INV-xx invariants with `and`.
- Each bounded temporal property TP-xx as a state property named
  `tpNN<Name>`, computed by composing the pure `*Op` functions on `current`
  (e.g. `finishOp(flushOp(fetchOp(current).sys).sys)`); kept out of
  `safetyInvariant`.
- No implementation details: no C++ class/method names or signatures (the
  mapping stays in the design doc).
- Inline comments mapping each element back to the design doc: section,
  operation name, owning component, INV/TP/BG/W IDs.

StatsFrameworkVerification:
- Model constant values as `pure val MODEL_*`, plus small finite domains for
  operation arguments (including invalid values). Sizes (capacities, number
  of identifiers) just above every threshold in the logic; values reduced to
  the equivalence classes the logic distinguishes.
- `action init = specInit`.
- An `anyCall = any { ... }` action listing all `do*` actions (arguments
  chosen with `nondet x = SET.oneOf()`), whether or not they can succeed.
- A `step = any { ... }` action with guarded branches (`all { guard, doX }`),
  so that most steps are operations that can succeed; branches that should be
  picked more often listed several times (weights); and one `anyCall` branch,
  so rejected calls still appear in traces. Guards here only choose test
  inputs; spec actions stay unguarded.
- A `stepTlc = any { ... }` action for `quint verify --backend=tlc
  --step=stepTlc`: each `do*` action once, reduced argument domains
  (`TLC_*` constants), and a guard on every action that grows a counter, so
  that counters stay within small bounds and the state space is finite.
- A `stepApalache = any { ... }` action for `quint verify --step=stepApalache
  --max-steps=N`: each `do*` action once, integer arguments taken from `Int`
  (symbolic) instead of a finite set.
- Naming rule for all steps (`anyCall`, `step`, `stepTlc`, `stepApalache`):
  every branch calls exactly one `do<Op>` action, with arguments chosen by
  `nondet` inline in the branch, not in a separately named helper action.
  `quint run --mbt` records the name of the action chosen in the step as
  `mbt::actionTaken`; with this rule traces contain only `do<Op>` names, which
  map 1:1 to the operations of the design doc.
- A scenario test for every boundary of a validity rule whose values are
  reduced in `stepTlc`.
- `val allProperties = safetyInvariant and <all tpNN...>`.
- A `val` per witness W-xx, named `wNN<Name>`, with the scenario in POSITIVE
  form (true once it has happened), for `quint run --witnesses wNN<Name> ...`.
  A trace of one witness is obtained with `--invariant='not(wNN<Name>)'`.
- A `run <name>Test` per witness plus one for rejections (BG-04), using
  `.then(...)` and `.expect(...)`; test names must end with `Test`.

Validate: `quint typecheck`, `quint test`,
`quint run --invariant=allProperties --witnesses <all wNN...>` (expect no
violation and N > 0 for every witness; check that no witness is near 0%), and
`quint verify` with `--step=stepTlc --backend=tlc` and with
`--step=stepApalache` (expect no violation; report the run time).
```

### 3.4 Changes compared to the original example prompt
| Original item | Changed? | Why | How |
|---|---|---|---|
| A typed var for each state variable | kept, extended | Pure functions need the whole state as one value; MBT needs operation results; some witnesses need history | Plus record type `Sys`, `var lastResult: OpResult`, and `ghost*` variables |
| An `init` action setting all vars | kept, moved | The spec module has unset `const`s and cannot be run; `quint run` needs `init` in the module passed to `--main` | `specInit` in the spec sets all vars; `init = specInit` in the verification module |
| One action per operation, with guards and primed assignments | yes | Rejections are observable behavior (BG-01, BG-04); with guards a rejected call never appears in a trace, so MBT would not test it. TP-01 needs operation logic it can call "on a copy", which only pure functions allow | Logic in pure `<op>Op` functions returning `Transition`; action `do<Op> = applyTransition(<op>Op(current))`, always enabled; failure recorded in `lastResult` |
| An unchanged assignment for every variable NOT modified | yes (equivalent) | Follows from the previous row: one shared writer instead of per-action assignments | `applyTransition` assigns every variable; unmodified ones are carried over in `t.sys` |
| A `val` per invariant named with its INV-ID | adjusted | Quint identifiers cannot contain `-`, so `INV-01` is not a valid name | `invNN<Name>` (e.g. `inv01BoundedCollected`) plus a comment with the exact ID |
| `step = any { ... }` listing all non-init actions | kept, moved, extended | `step` chooses which calls to explore and with which arguments (small model domains): a verification concern, not behavior. A uniform choice among always-enabled actions gave low coverage | `step` in `StatsFrameworkVerification`, arguments chosen with `nondet ... oneOf()`; guarded and weighted branches plus one `anyCall` branch (all actions, unguarded); additional `stepTlc` and `stepApalache` for exhaustive checking, because the standard `quint verify` with the simulation `step` did not finish |
| Inline comments mapping elements to INV-IDs | kept, extended | Traceability for operations and witnesses too | Comments with section, operation name, owning component and INV/TP/BG/W IDs |
| *(not in original)* | added | The design doc grew beyond the queue example | `lastResult`/`OpResult`, `ghost*` naming rule, temporal properties, witnesses as positive `val`s checked with `--witnesses`, scenario tests, "no implementation details" rule, validation commands |

### 3.5 Limitation: witnesses cannot be grouped
Invariants can be grouped into one `val` (`allProperties = safetyInvariant and
tp01StopTerminates`) and passed to `--invariant` by a single name. **Witnesses
cannot be grouped in a useful way**, so every witness has to be listed by
name in `--witnesses`, and Quint itself does not fail when a witness is never
reached (it only reports the count). There is no good solution for this.

Why grouping does not work (measured, 10,000 traces):
- `and { w01…w07 }` → **0%**: `and` requires all scenarios in the **same
  state**, but they happen at different times and some exclude each other
  (W-04 needs `lost > 0`, W-06 needs `lost = 0`).
- `or { w01…w07 }` → 74.70%: only says that **some** scenario happened; a
  witness that is never reached stays hidden.

An invariant must hold in every state, so "all at once" is exactly what is
wanted; a witness must happen at least once, each at its own time, so it needs
its own result.

Workarounds considered, all weak:
- **List in a shell script / Makefile** (`WITNESSES=...`, plus a check that
  every witness has N > 0): works, but the list lives outside the spec and
  has to be kept in sync by hand.
- **`.expect(wNN)` in each scenario test** (`quint test` then checks every
  witness): deterministic and needs no list, but only proves reachability by
  one hand-written path; it does not measure coverage, and the tests have to
  be written per witness anyway.
- **Latching `ghost` flags per witness and one group checked at the end of a
  trace:** requires all scenarios in **one** trace (rare; W-04 and W-06
  practically exclude each other) and adds a state variable per witness.

Current state: the witness list is written in the command
(`--witnesses w01HolderFull … w07FlusherFull`), and the result has to be read
by eye (every witness N > 0). Be aware of it when adding a witness: it must
also be added to the command.

## 4. C++ implementation

### 4.1 Prompt for generating the C++ code
Produces the implementation (Phase 4 of the workflow) from the design document
and the spec. The MBT harness is a separate step (see 5).

```
Generate the C++ implementation of the stats framework in stats_framework/.

Sources of truth:
- docs/statsDESIGN.md: "Operations", "Flusher Conversion Rules", "Queries"
  and "Spec to Implementation Mapping (C++)" (names, signatures, visibility,
  build and layout);
- spec/stats.qnt, module StatsFramework: the exact behavior of every
  "Succeeds when / Rejected when" branch (pure functions <op>Op).
Do NOT implement anything from "Verification Only" or from the module
StatsFrameworkVerification: no ghost variables, no stored lastResult, no
step actions, no witnesses, no model constants.

Build and layout:
- C++17, CMake (CMakeLists.txt in stats_framework/); library target from
  src/, test executable from tests/.
- src/: one .h and one .cpp per class; shared types (CollectorId,
  ProcedureId, StatRecord, OrchestratorState, StatsConfig) in one header;
  IStatsRepository as an interface header.
- tests/: GoogleTest (via CMake FetchContent), the in-memory fake repository
  with setAvailable(b), StatsTestAccess.

API: exactly as in the mapping table (class and method names, signatures,
return types, public vs. test-only). In particular:
- start(), stop(), finish(), flush() return bool (success / rejected);
  finish() is public;
- storeStatsRecord() returns void (fire-and-forget);
- fetch() returns std::optional<size_t>: std::nullopt when rejected, the
  number of samples moved to processing (may be 0) on success;
- tick() calls fetch(), flush() and, while stopping, finish(); it adds no
  behavior of its own.

Behavior: implement every branch of every operation exactly as in the design
doc (same conditions, same effects). A rejected operation changes nothing
except the effect its branch lists. Operations may be called in any order
(MBT calls fetch(), flush() and finish() directly), so no method may assume
the order used by tick().

Encapsulation: all data members private. Only the accessors listed as public
in "Visibility of accessors" are public. StatsHolder and StatsFlusher declare
`friend class StatsTestAccess;`; this declaration is the only test-related
code allowed in src/. Rejections are return values, not exceptions.

The "not running" check of StoreStatsRecord is in the holder, driven by the
orchestrator, as described in "Who checks that the orchestrator is running":
the collector depends only on the holder and never on the orchestrator.

Where the design doc leaves any other choice, choose the simplest solution
that keeps the operations independent, and state the choice in a comment.

Comments: each operation cites its design-doc operation and branch (e.g.
"// Operation: Fetch — Rejected when ..."), and each counter its design-doc
state variable.

Tests in this step: GoogleTest unit tests for the success and every
rejection branch of each operation (mirroring the scenario tests in
spec/stats.qnt), and one test that tick() behaves like fetch(), flush()
(and finish() while stopping).

Validate: configure and build with CMake, run the unit tests; all must pass.
```

## 5. MBT harness (C++)

### 5.1 Notes for the harness prompt
Found while inspecting traces generated with
`quint run --mbt --out-itf=...`. The prompt for generating the C++ MBT harness
must make the harness handle these points:
- **Variable names are fully qualified** with the module path, e.g.
  `StatsFrameworkVerification::StatsFramework::collected`. The harness strips
  the prefix (everything up to the last `::`) before mapping a variable to its
  C++ accessor.
- **Verification-only data is not compared:** all `ghost*` variables, and in
  `lastResult` the label `StoreStatsRecordResult(...)` (the C++ method returns
  `void`). The other `lastResult` variants are compared with the value
  returned by the C++ method (BG-01).
- **Which operation to call:** `mbt::actionTaken` holds the `do<Op>` name of
  the step (`init` for the initial state), and `mbt::nondetPicks` holds its
  arguments (`c`, `p`, `d` for `doStoreStatsRecord`, `b` for
  `doSetRepositoryAvailable`; unused ones are `None`). Each name maps to one
  C++ method according to "Spec to Implementation Mapping (C++)" in
  statsDESIGN.md.
- **ITF value encoding:** sum types as `{"tag": "...", "value": ...}` (e.g.
  `{"tag": "FlushResult", "value": false}`, `Option` picks as
  `{"tag": "Some", "value": ...}` / `{"tag": "None", ...}`), maps as
  `{"#map": [[key, value], ...]}`, tuples as `{"#tup": [...]}`, large
  integers as `{"#bigint": "..."}`, lists as JSON arrays, records as JSON
  objects.
- **Any order of operations:** traces may call `fetch()`, `flush()` and
  `finish()` in orderings `tick()` never produces (e.g. two `fetch()` without
  a `flush()`); the harness calls them directly, one per trace step.
- **Repository availability** (`doSetRepositoryAvailable`) is driven through
  the in-memory fake repository (`setAvailable(b)`), not through the
  framework.

Trace generation (`scripts/gen_traces.sh`): 500 random traces × 30 steps (as
in the workflow document) plus one targeted trace per witness
(`--invariant='not(wNN...)' --mbt`), so that rare scenarios (W-02, W-07: ~3% of
random traces) are certainly replayed. Traces are test cases and are committed
to the repository, so the script makes them reproducible: fixed seed, and
`#meta` normalized (creation time removed, absolute spec path made relative),
because otherwise every regeneration changes every file. Witness traces carry
`"status": "violation"` in `#meta`; for the harness this means "scenario
reached", not a failure.

### 5.2 Prompt for generating the MBT harness
Produces the harness used in this project (`tests/mbt_test.cpp`).

```
Generate a GoogleTest MBT harness in stats_framework/tests/mbt_test.cpp that
replays every ITF trace in traces/random/ and traces/witness/ against the C++
implementation (one parameterized test per trace file; trace directory passed
from CMake as a compile definition).

For every trace: build a fresh framework with the model values of
StatsFrameworkVerification (holder 2, flusher 3, MAX_DURATION_US 10), create
collectors on demand by id. State 0 must have mbt::actionTaken = "init".
For every state:
- execute the operation named in mbt::actionTaken (do<Op> -> C++ method from
  the mapping table in docs/statsDESIGN.md) with arguments from
  mbt::nondetPicks; doSetRepositoryAvailable drives the fake repository;
- BG-01: compare the value returned by the C++ method with lastResult
  (fetch(): ok=false <-> std::nullopt, else n); skip the
  StoreStatsRecordResult label (void method);
- BG-02: compare state, collected, processing (via StatsTestAccess), stored
  and repoAvailable (via the fake repository) and the counters dropped,
  overflowed, discarded, lost; do not compare ghost* variables or nextSeq.
Handle everything listed in 5.1 (qualified variable names, ITF encoding,
any order of operations). On the first difference fail with the trace file,
step number, action, compared field, expected and actual value.

Validate: build, run the harness on all traces (all pass), then check that
the harness detects bugs: inject a few deliberate bugs in src/ one at a time
(e.g. a counter not incremented, a wrong branch), confirm that traces fail,
and restore the sources.
```

### 5.3 Result
- Unit tests: 19 tests, all pass.
- MBT: 507 traces (500 random + 7 witness) replayed, all pass (~2.5 s; `ctest` runs all 527 tests in ~7 s).
- Mutation check (each bug injected alone, sources restored afterwards):

| Injected bug | Traces that detect it | First difference reported |
|---|---|---|
| holder does not count dropped samples | 225 / 507 | `dropped[c1]`: expected 1, actual 0 |
| failed `Flush` while `Stopping` keeps the samples instead of losing them | 274 / 507 | `processing` not empty |
| holder keeps accepting samples while `Stopping` | 192 / 507 | `collected` contains a sample that should have been ignored |
| `Fetch` ignores the flusher capacity | 47 / 507 | `FetchResult`: expected 1, actual 2 |

A green MBT run alone does not show that the harness compares anything; the
mutation check does. The rarest bug (flusher overflow) is still detected by
47 traces, thanks to the improved `step` (coverage) and the witness traces.

Design choice made while implementing: the flusher also needs the
orchestrator state (Fetch rejected when Stopped; a failed Flush loses samples
only while Stopping). It follows the same pattern as the holder: the
orchestrator drives it (`StatsFlusher::setOrchestratorState`), so neither the
flusher nor the holder depends on the orchestrator. Recorded in the mapping
section of statsDESIGN.md.
