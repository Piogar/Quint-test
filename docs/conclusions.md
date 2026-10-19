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
1. **Friend test-access class (preferred).** e.g. `StatsHolder` declares
   `friend class StatsHolderTestAccess;`; that class exists only in the test
   code and reads the private data. Production API stays clean, no
   build-specific code; cost is one `friend` line per class.
2. **Protected members + test subclass.** Works, but forces `protected` where
   `private` would be enough.
3. **`#ifdef TESTING` accessors.** Production build differs from the tested
   build. Avoid.
4. **No snapshots; observe only through public behavior.** MBT checks only
   counts, counters and `stored` (via the fake repository). Weaker: e.g. an
   ordering bug in `collected` is detected only after the next `Flush`.

**Direction:** keep `state()`, counts and counters public; give tests access
to the snapshots only through a friend test-access class. Not yet reflected in
the mapping section of `statsDESIGN.md`.

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
- **Logic in pure functions, steps in actions** (see 3.2).
- **`ghost*` variables** exist only for verification (e.g. history flags for
  witnesses that describe a sequence of events). They have no counterpart in
  the implementation and are not compared by MBT.
- **`step` belongs to verification.** It chooses which calls to explore and
  with which arguments (small model domains, including invalid values); the
  spec only defines what each call does.
- **Witnesses are negated `val`s.** `quint run --invariant=wNN...` reports a
  violation when the scenario is reachable; the trace is the witness and a
  targeted MBT test. Consequence: with `--invariant`, a witness result is read
  the other way round ("Invariant violated" = success, "No violation found" =
  scenario not reached), while for real invariants a violation is a bug. To
  avoid confusing the two, check invariants via `--invariant` and witnesses in
  **positive** form via `--witnesses` (the run stays green and reports how many
  traces reached each scenario, which also shows coverage). Commands and
  details: [quintCommands.md](quintCommands.md), section "How to use
  `--invariant` and `--witnesses`".
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
  operation arguments (including invalid values).
- `action init = specInit`.
- A `step = any { ... }` action listing all `do*` actions, choosing arguments
  with `nondet x = SET.oneOf()`.
- `val allProperties = safetyInvariant and <all tpNN...>`.
- A `val` per witness W-xx, named `wNN<Name>`, written as the NEGATION of the
  scenario, so `quint run --invariant=wNN<Name>` reports a violation when the
  scenario is reachable (the trace is the witness).
- A `run <name>Test` per witness plus one for rejections (BG-04), using
  `.then(...)` and `.expect(...)`; test names must end with `Test`.

Validate: `quint typecheck`, `quint test`, `quint run --invariant=allProperties`
(expect no violation), and `quint run --invariant=wNN...` for each witness
(expect a violation).
```

### 3.4 Changes compared to the original example prompt
| Original item | Changed? | Why | How |
|---|---|---|---|
| A typed var for each state variable | kept, extended | Pure functions need the whole state as one value; MBT needs operation results; some witnesses need history | Plus record type `Sys`, `var lastResult: OpResult`, and `ghost*` variables |
| An `init` action setting all vars | kept, moved | The spec module has unset `const`s and cannot be run; `quint run` needs `init` in the module passed to `--main` | `specInit` in the spec sets all vars; `init = specInit` in the verification module |
| One action per operation, with guards and primed assignments | yes | Rejections are observable behavior (BG-01, BG-04); with guards a rejected call never appears in a trace, so MBT would not test it. TP-01 needs operation logic it can call "on a copy", which only pure functions allow | Logic in pure `<op>Op` functions returning `Transition`; action `do<Op> = applyTransition(<op>Op(current))`, always enabled; failure recorded in `lastResult` |
| An unchanged assignment for every variable NOT modified | yes (equivalent) | Follows from the previous row: one shared writer instead of per-action assignments | `applyTransition` assigns every variable; unmodified ones are carried over in `t.sys` |
| A `val` per invariant named with its INV-ID | adjusted | Quint identifiers cannot contain `-`, so `INV-01` is not a valid name | `invNN<Name>` (e.g. `inv01BoundedCollected`) plus a comment with the exact ID |
| `step = any { ... }` listing all non-init actions | kept, moved | `step` chooses which calls to explore and with which arguments (small model domains): a verification concern, not behavior | `step` in `StatsFrameworkVerification`, arguments chosen with `nondet ... oneOf()` |
| Inline comments mapping elements to INV-IDs | kept, extended | Traceability for operations and witnesses too | Comments with section, operation name, owning component and INV/TP/BG/W IDs |
| *(not in original)* | added | The design doc grew beyond the queue example | `lastResult`/`OpResult`, `ghost*` naming rule, temporal properties, witnesses as negated `val`s, scenario tests, "no implementation details" rule, validation commands |
