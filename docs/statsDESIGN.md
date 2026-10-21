# Stats Framework Design

## Overview
Framework used to collect performance statistics for various procedures executed across the system. Framework consists of following components:
- StatsOrchestrator - orchestrates whole flow. Driven periodically by the application (`tick()`).
- StatsCollector - worker objects spread around source code. Used as entry point to gather single perfomance samples.
- StatsHolder - handles and stores data samples collected from the system
- StatsFlusher - triggered by StatsOrchestrator. Can modify and/or convert data to format understandable by target data repository.

Once data is collected and processed, it is stored in target data repository (e. g. DB).
All components run on a single application thread (see Execution model).

### Sample lifecycle
| Where the sample is | Stage | State variable |
|---------------------|-------|----------------|
| StatsHolder         | collected  | `collected`  |
| StatsFlusher        | processing | `processing` |
| StatsRepository     | stored     | `stored`     |

A sample can also leave the flow without being stored: **dropped** (holder
full), **overflowed** (flusher full at `Fetch`), **discarded** (invalid) or
**lost** (repository unavailable during shutdown).

### Data flow
```
                                                       StatsOrchestrator (tick())
                                                         | triggers Fetch / Flush
                                                         v
StatsCollector --StoreStatsRecord--> StatsHolder --Fetch--> StatsFlusher --Flush--> StatsRepository
                                     [collected]            [processing]  (convert/   [stored]
                                      (bounded)                            validate)
```
- Collectors only append samples to the StatsHolder's bounded `collected`
  sequence. They never block and never talk to the flusher or repository
  directly.
- The orchestrator triggers the StatsFlusher. The flusher fetches all samples
  currently collected in the holder (`Fetch`), leaving the holder empty,
  converts them to the repository format and writes the result to the
  repository (`Flush`).
- The repository may be temporarily unavailable. Samples that could not be
  written stay in processing and are retried; they are never lost or reordered.
  Later fetches append behind them. Processing is bounded by
  `FLUSHER_CAPACITY`; samples that do not fit at `Fetch` are overflowed
  (dropped and counted).
  The only exception is shutdown: while stopping, samples that cannot be
  written are removed and counted as lost, so that stopping always completes.
- Once flush is triggered, current samples are marked as being processed. Upcoming samples are not included in current flush window.

### Execution model (single-threaded)
The framework is single-threaded. All operations, including
collectors' `StoreStatsRecord` calls and the orchestrator's `Fetch`, `Flush`
and `Finish`, run on one application thread, one after another. The
orchestrator has no thread of its own; the application drives it by calling
`StatsOrchestrator::tick()` periodically (e.g. once per main-loop iteration).
No locking is required.

The spec models every operation as a single step and allows any order of
steps, so it does not change when threads are added (see Phase 2 at the end of
this document).

## Types
- `CollectorId`: identifier of a StatsCollector instance (element of `COLLECTORS`).
- `ProcedureId`: identifier of a measured procedure (element of `PROCEDURES`).
- `StatRecord`: `{ collector: CollectorId, procedure: ProcedureId, seq: int, durationUs: int }`
  - One performance sample. The same type is used in all three stages
    (`collected`, `processing`, `stored`).
  - `seq` is the per-collector sequence number assigned at `StoreStatsRecord`.
  - If the target repository needs a different format (e.g. timestamps, unit
    conversion), the repository adapter builds it when writing; that format is
    not part of the spec.
- `OrchestratorState`: `Stopped | Running | Stopping`.

## Abstract State
- `state` (OrchestratorState): lifecycle of the orchestrator. Initially `Stopped`.
- `collected` (sequence of StatRecord): samples collected in the StatsHolder,
  i.e. accepted by `StoreStatsRecord` but not yet processed, oldest first.
  Initially empty.
- `processing` (sequence of StatRecord): samples fetched by the StatsFlusher
  and not yet written to the repository, oldest first. Initially empty.
- `stored` (sequence of StatRecord): samples stored in the target repository,
  in storage order. Initially empty.
- `nextSeq` (CollectorId → int): next sequence number for each collector.
  Initially `0` for every collector.
- `dropped` (CollectorId → int): samples rejected because the holder was full.
  Initially `0`.
- `overflowed` (CollectorId → int): samples dropped at `Fetch` because
  `processing` was full. Initially `0`.
- `discarded` (CollectorId → int): samples rejected by the flusher as invalid.
  Initially `0`.
- `lost` (CollectorId → int): samples removed by the flusher because the
  repository was unavailable during shutdown. Initially `0`.
- `repoAvailable` (bool): whether the repository currently accepts writes.
  Controlled by the environment. Initially `true`.

## Constants
All constants are fixed when the framework is created and never change.
- `COLLECTORS` (set of CollectorId): collectors that exist in the system.
- `PROCEDURES` (set of ProcedureId): procedures that can be measured.
- `HOLDER_CAPACITY` (int, > 0): maximum number of samples the StatsHolder keeps
  in `collected`.
- `FLUSHER_CAPACITY` (int, > 0): maximum number of samples the StatsFlusher
  keeps in `processing`.
- `MAX_DURATION_US` (int, ≥ 0): largest duration the flusher accepts.

## Flusher Conversion Rules
Conversion is a pure function of the samples in processing; apart from
`processing` and the `overflowed`, `discarded` and `lost` counters the flusher
has no state of its own.
- A sample is **valid** when `0 ≤ durationUs ≤ MAX_DURATION_US`.
- `convert(processing)` returns the valid samples of `processing`, unchanged
  and in order.
- Invalid samples are left out and counted in `discarded`.

## Operations
Each operation either succeeds or is rejected. Both outcomes are part of the
required behavior. Unless stated otherwise, a rejected operation leaves the
whole state unchanged.

All operations except `StoreStatsRecord` report their outcome to the caller.
`StoreStatsRecord` returns nothing: collecting stats is fire-and-forget, and
callers must not react to a lost sample. Its outcomes differ only in their
effect on the state, which the framework exposes through the `dropped`
counters and the queries.

### Start (caller: application)
- **Succeeds when**: `state = Stopped`
  - **Effect**: `state := Running`.
  - **Result**: success.
- **Rejected when**: `state ≠ Stopped`
  - **Result**: failure.

### StoreStatsRecord(c, p, d) (caller: StatsCollector `c`)
Collects one sample of duration `d` microseconds for procedure `p` into the
StatsHolder.
- **Succeeds (accepted) when**: `state = Running` and `length(collected) < HOLDER_CAPACITY`
  - **Effect**: `{collector: c, procedure: p, seq: nextSeq[c], durationUs: d}`
    is appended to the back of `collected`; `nextSeq[c] := nextSeq[c] + 1`.
- **Rejected (ignored) when**: `state ≠ Running`
  - **Effect**: none.
- **Rejected (dropped) when**: `state = Running` and `length(collected) = HOLDER_CAPACITY`
  - **Effect**: `dropped[c] := dropped[c] + 1`; nothing else changes
    (in particular `nextSeq[c]` is not incremented).
- **Result**: none in all cases.

Note: `d` is not validated here. Validation is the flusher's job, so invalid
durations are accepted into `collected` and discarded later by `Flush`.

### Fetch (caller: StatsFlusher, triggered by the orchestrator)
Moves all samples currently collected in the StatsHolder to the flusher,
behind any samples still waiting in processing (e.g. after a failed `Flush`).
The holder is always left empty. Samples collected afterwards stay in the
holder for the next `Fetch`.
- **Succeeds when**: `state ∈ {Running, Stopping}` and `collected` is not empty
  - **Effect**: let `k = min(length(collected), FLUSHER_CAPACITY - length(processing))`.
    The first `k` samples of `collected` are appended to the back of
    `processing`, in order. For each remaining sample `s` of `collected`,
    `overflowed[s.collector]` is incremented. `collected` becomes empty.
  - **Result**: success (number of samples moved to processing, `n = k`;
    `n` may be `0` if processing was already full).
- **Rejected when**: `state = Stopped`, or `collected` is empty
  - **Result**: failure.

### Flush (caller: StatsFlusher, triggered by the orchestrator)
Converts the samples in processing and writes the result to the repository.
- **Succeeds when**: `processing` is not empty and `repoAvailable = true`
  - **Effect**: the samples `convert(processing)` are appended to the back of
    `stored`, in order; for each invalid sample `s` in `processing`,
    `discarded[s.collector]` is incremented; `processing` becomes empty.
  - **Result**: success.
- **Rejected when**: `processing` is empty
  - **Result**: failure (nothing to flush).
- **Rejected when**: `processing` is not empty, `repoAvailable = false` and
  `state = Running`
  - **Effect**: none; `processing` is kept for a later retry.
  - **Result**: failure (write failed).
- **Rejected when**: `processing` is not empty, `repoAvailable = false` and
  `state = Stopping`
  - **Effect**: for each sample `s` in `processing`, `lost[s.collector]` is
    incremented; `processing` becomes empty. Nothing is added to `stored`.
  - **Result**: failure (write failed, samples removed).

`state = Stopped` with a non-empty `processing` cannot occur (INV-07).

The repository write is all-or-nothing: either every sample in processing is
stored or none is.

If the target repository limits how much can be written at once, the
StatsFlusher splits the write into smaller parts internally. This splitting
is an implementation detail and is not modeled in the spec; one `Flush` is
still one step.

### Stop (caller: application)
Stops accepting samples and starts draining: samples already collected or in
processing are still fetched and flushed. If the repository is unavailable
during draining, they are removed and counted in `lost` instead of being
retried, so `Finish` can always be reached.
- **Succeeds when**: `state = Running`
  - **Effect**: `state := Stopping`.
  - **Result**: success.
- **Rejected when**: `state ≠ Running`
  - **Result**: failure.

### Finish (caller: StatsOrchestrator)
Completes shutdown once everything has been drained.
- **Succeeds when**: `state = Stopping`, `collected` is empty and `processing`
  is empty
  - **Effect**: `state := Stopped`.
  - **Result**: success.
- **Rejected when**: `state ≠ Stopping`, or `collected` is not empty, or
  `processing` is not empty
  - **Result**: failure.

### SetRepositoryAvailable(b) (caller: environment)
Models the repository going down or coming back. Not called by the framework.
- **Always succeeds**
  - **Effect**: `repoAvailable := b`.
  - **Result**: success.

## Queries
Read-only. They never change the state. They are derived from the state, so
they are not separate state.

| Query | Returns | Meaning | Component | C++ |
|-------|---------|---------|-----------|-----|
| **State** | `state` | Lifecycle phase: `Stopped`, `Running` or `Stopping` | StatsOrchestrator | `StatsOrchestrator::state()` |
| **CollectedCount** | `length(collected)` | Samples collected but not yet processed | StatsHolder | `StatsHolder::collectedCount()` |
| **Dropped(c)** | `dropped[c]` | Samples of collector `c` rejected because the holder was full | StatsHolder | `StatsHolder::dropped(c)` |
| **Overflowed(c)** | `overflowed[c]` | Samples of collector `c` dropped at `Fetch` because the flusher was full | StatsFlusher | `StatsFlusher::overflowed(c)` |
| **ProcessingCount** | `length(processing)` | Samples being processed, not yet stored (e.g. waiting for a retry) | StatsFlusher | `StatsFlusher::processingCount()` |
| **Discarded(c)** | `discarded[c]` | Samples of collector `c` rejected as invalid during conversion | StatsFlusher | `StatsFlusher::discarded(c)` |
| **Lost(c)** | `lost[c]` | Samples of collector `c` removed because the repository was unavailable during shutdown | StatsFlusher | `StatsFlusher::lost(c)` |
| **StoredCount** | `length(stored)` | Samples stored in the target repository | StatsRepository | in-memory fake only (tests); not required from the production DB adapter |

## State Invariants
Properties of every single state. These are checked on the spec by the model
checker.
- **INV-01 BoundedCollected**: `0 ≤ length(collected) ≤ HOLDER_CAPACITY`.
- **INV-02 BoundedProcessing**: `0 ≤ length(processing) ≤ FLUSHER_CAPACITY`.
- **INV-03 Conservation**: for every collector `c`, `nextSeq[c]` equals the
  number of samples of `c` in `collected` plus those in `processing` plus
  those in `stored` plus `overflowed[c]` plus `discarded[c]` plus `lost[c]`.
  (Every accepted sample is accounted for exactly once.)
- **INV-04 UniqueSamples**: no pair `(collector, seq)` occurs more than once
  across `collected`, `processing` and `stored` together.
- **INV-05 PerCollectorOrder**: within `collected`, within `processing` and
  within `stored`, samples of the same collector appear with strictly
  increasing `seq`; moreover every `seq` of a collector in `stored` is smaller
  than every `seq` of that collector in `processing`, and those are smaller
  than every `seq` of that collector in `collected`.
- **INV-06 OnlyValidStored**: every sample in `stored` has
  `0 ≤ durationUs ≤ MAX_DURATION_US`.
- **INV-07 StoppedIsDrained**: `state = Stopped` implies `collected` and
  `processing` are empty.
- **INV-08 NonNegativeCounters**: `nextSeq[c]`, `dropped[c]`,
  `overflowed[c]`, `discarded[c]` and `lost[c]` are `≥ 0` for every
  collector `c`.

### Temporal property
Checked on the spec, not by model-based testing (replaying a finite trace
cannot show that something eventually happens).
- **TP-01 StopTerminates**: from any state with `state = Stopping`, `Stopped`
  is reached by at most three orchestrator steps (`Fetch`, `Flush`, `Finish`)
  when no other operation intervenes, whether or not the repository is
  available. In production this means that continuing to call `tick()` after
  `Stop` always reaches `Stopped`.

## Behavioral Guarantees
Properties the implementation must satisfy over a sequence of operations. They
are checked by model-based testing: traces generated from the spec are replayed
against the implementation.
- **BG-01 ResultConformance**: every operation that reports a result returns
  the same result as the spec (success/failure, `n` for Fetch).
  `StoreStatsRecord` returns nothing; its outcome is checked through BG-02.
- **BG-02 StateConformance**: after every operation, the implementation's
  `state`, collected, processing and stored samples, and the `dropped`,
  `overflowed`, `discarded` and `lost` counters equal the spec's, element by
  element and in order. `nextSeq[c]` is internal to the collector and is not
  compared directly; it is checked through the `seq` values of the samples.
- **BG-03 EndToEndFIFO**: samples are stored in exactly the order in which
  the corresponding valid samples were accepted by `StoreStatsRecord`.
- **BG-04 Rejection**: a rejected operation reports the specified failure (if
  it reports a result) and changes nothing except what its Effect lists (only
  a dropped `StoreStatsRecord` changes state, namely `dropped[c]`, and a
  failed `Flush` while stopping, which moves the samples in processing into
  `lost`).
- **BG-05 RetryWithoutLoss**: while `Running`, a `Flush` that fails because
  the repository is unavailable keeps `processing` unchanged; later fetches
  only append behind those samples, and the next successful `Flush` stores
  them first, in order.
- **BG-05a LoseOnStopWhenUnavailable**: while `Stopping`, a `Flush` that
  fails because the repository is unavailable removes all samples in
  `processing`, adds each to `lost[c]` of its collector, writes nothing to
  `stored`, and leaves `processing` empty so that `Finish` can follow.
- **BG-06 GracefulStop**: after `Stop`, no new samples are accepted, and
  `Finish` succeeds only after every previously accepted sample has been
  stored, overflowed, discarded or lost.

Termination of shutdown is a temporal property and is checked on the spec
(TP-01), not here.

## Spec to Implementation Mapping (C++)
| Spec element                 | Implementation element |
|------------------------------|------------------------|
| `CollectorId`, `ProcedureId` | `using CollectorId = std::string;`, `using ProcedureId = std::string;` (the same string values as in the traces, e.g. `"c1"`, `"p1"`) |
| `state`                      | `StatsOrchestrator::state()` returning `enum class OrchestratorState { Stopped, Running, Stopping }` |
| `collected`                  | bounded sample sequence inside `StatsHolder` (private); read by tests only, through `StatsTestAccess` |
| `processing`                 | samples currently held by `StatsFlusher` (private); read by tests only, through `StatsTestAccess` |
| `stored`                     | contents of the `IStatsRepository` implementation (DB adapter in production, in-memory fake in tests); read-only snapshot via the fake |
| `repoAvailable`              | availability of the `IStatsRepository`; `write()` returns `false` when unavailable |
| `nextSeq[c]`                 | sequence counter inside `StatsCollector` instance `c` |
| `dropped[c]`                 | per-collector counter exposed by `StatsHolder::dropped(c)` |
| `overflowed[c]`              | per-collector counter exposed by `StatsFlusher::overflowed(c)` |
| `discarded[c]`               | per-collector counter exposed by `StatsFlusher::discarded(c)` |
| `lost[c]`                    | per-collector counter exposed by `StatsFlusher::lost(c)` |
| `StatRecord`                 | `struct StatRecord { CollectorId collector; ProcedureId procedure; int64_t seq; int64_t durationUs; }` |
| `COLLECTORS`                 | `StatsCollector` instances, each with a `CollectorId`, writing to a shared `StatsHolder` |
| `PROCEDURES`                 | `ProcedureId` values passed to `StatsCollector::storeStatsRecord` |
| `HOLDER_CAPACITY`, `FLUSHER_CAPACITY`, `MAX_DURATION_US` | fields of `StatsConfig` used to construct `StatsHolder` and `StatsFlusher` |
| Start / Stop                 | `bool StatsOrchestrator::start()` / `bool StatsOrchestrator::stop()` |
| StoreStatsRecord(c, p, d)    | `void StatsCollector::storeStatsRecord(ProcedureId p, int64_t durationUs)` on collector `c`, appending to `StatsHolder`; the holder ignores it when not accepting (see "Who checks that the orchestrator is running") |
| Fetch                        | `std::optional<size_t> StatsFlusher::fetch()` taking all collected samples from `StatsHolder` |
| Flush                        | `bool StatsFlusher::flush()` calling `convert()` then `IStatsRepository::write()` |
| Finish                       | `bool StatsOrchestrator::finish()`, public (the application may call it; `tick()` calls it while stopping; MBT calls it directly) |
| Flusher conversion rules     | `std::vector<StatRecord> StatsFlusher::convert(const std::vector<StatRecord>&) const`: pure, returns the valid samples unchanged and in order; `flush()` counts the invalid ones in `discarded` |
| SetRepositoryAvailable(b)    | `setAvailable(b)` on the in-memory fake repository (test only) |

In production the application calls `void StatsOrchestrator::tick()`
periodically. One tick triggers the flusher's `fetch()` and `flush()` and,
while stopping, calls `finish()`. `tick()` is a convenience that only composes
the spec operations above; it adds no behavior of its own. For model-based
testing the test calls `fetch()`, `flush()` and `finish()` directly, one at a
time, so that traces can be replayed deterministically. Traces may call them
in any order, including orderings `tick()` never produces (e.g. two
`fetch()` calls without a `flush()` in between); the implementation must
behave as specified for every such ordering.

Anything in the spec that is not listed here exists only for verification and
must not appear in the implementation.

### Who checks that the orchestrator is running
`StoreStatsRecord` is ignored when `state ≠ Running` (design: "Rejected
(ignored)"; BG-06, INV-07 depend on it). The collector does **not** know or
call the orchestrator; it only appends to the `StatsHolder`. The check is in
the holder, and the orchestrator drives it:
- `StatsHolder::setAccepting(bool)`: `StatsOrchestrator::start()` sets it to
  `true` on success, `StatsOrchestrator::stop()` sets it to `false` on
  success. Initially `false`.
- `StatsHolder` ignores a sample when not accepting (no change at all, the
  collector's sequence counter is not incremented either).
- Hence "holder accepting" ⇔ `state = Running` at all times; the spec state
  `state` stays owned by the orchestrator.
- The collector learns the outcome from the holder (accepted / ignored /
  dropped) only to decide whether to increment its sequence counter; the
  outcome is not returned to the caller of `storeStatsRecord`.

The flusher needs the orchestrator state too (`Fetch` is rejected when
`Stopped`; a failed `Flush` loses the samples only while `Stopping`). Same
pattern: the orchestrator drives it, the flusher does not depend on the
orchestrator.
- `StatsFlusher::setOrchestratorState(OrchestratorState)`: called by the
  orchestrator whenever its state changes (construction, `start()`, `stop()`,
  `finish()`). Initially `Stopped`.

Dependencies: collector → holder, orchestrator → holder, orchestrator →
flusher, flusher → holder and repository. No component depends on the
collector or on the orchestrator, and the collector depends on nothing but
the holder.

### Visibility of accessors
| Accessor | Visibility | Why |
|---|---|---|
| `StatsOrchestrator::state()` | public | the application checks whether shutdown completed |
| `StatsHolder::collectedCount()`, `StatsHolder::dropped(c)` | public | health metrics and monitoring |
| `StatsFlusher::processingCount()`, `overflowed(c)`, `discarded(c)`, `lost(c)` | public | health metrics and monitoring |
| contents of `collected` and `processing` | **test only**, via `StatsTestAccess` | MBT compares them after every step (BG-02); production must not see the samples inside the holder and the flusher |
| `StatsHolder::setAccepting(bool)` | public, but called only by `StatsOrchestrator` (`start()`, `stop()`) | internal wiring; applications must not call it |
| `StatsFlusher::setOrchestratorState(OrchestratorState)` | public, but called only by `StatsOrchestrator` | internal wiring; applications must not call it |
| `setAvailable(b)` | test only | exists only on the in-memory fake repository |

Test access: `StatsHolder` and `StatsFlusher` declare
`friend class StatsTestAccess;`. `StatsTestAccess` is defined only in
`tests/` and returns read-only copies of the private sample sequences. The
production code contains only the `friend` declaration, and the production and
test builds are the same.

### Build and layout
- C++17, built with CMake.
- Production sources (`.h` and `.cpp`): `stats_framework/src/`.
- Tests (unit tests, `StatsTestAccess`, in-memory fake repository, MBT
  harness): `stats_framework/tests/`, using GoogleTest; the MBT harness reads
  ITF traces with nlohmann/json.
- Traces: `stats_framework/traces/` (generated by `scripts/gen_traces.sh`).
- Dependencies GoogleTest and nlohmann/json are downloaded by CMake
  (`FetchContent`).
- Build and run (from `stats_framework/`):
  ```bash
  cmake -S . -B build && cmake --build build -j
  ./build/stats_unit_tests     # unit tests: every branch of every operation
  ./build/stats_mbt_tests      # MBT: replays all traces in traces/
  ```
  (or `ctest --test-dir build` for both).
- All checks of the project (Quint, traces up to date, C++ build and tests):
  `scripts/check_all.sh` (see README.md).

## Verification Only (not implementation behavior)
- Model constants: `COLLECTORS = {"c1", "c2"}`, `PROCEDURES = {"p1", "p2"}`,
  `HOLDER_CAPACITY = 2`, `FLUSHER_CAPACITY = 3`, `MAX_DURATION_US = 10`.
  How they are chosen (the model must be as small as possible, but must still
  show every threshold in the logic):
  - **Sizes** (how many of something) are bounded just above every threshold
    that appears in a guard or condition. `HOLDER_CAPACITY = 2` is enough to
    reach "holder full"; `FLUSHER_CAPACITY = 3` is larger than the holder, so
    that a `Fetch` can move only **part** of the collected samples (the case
    between "all fit" and "none fit"). Two collectors are needed to see the
    samples of different collectors interleave (per-collector counters,
    INV-05); a third adds no new behavior. Larger values only add states.
  - **Values** (what is passed) are reduced to their equivalence classes,
    i.e. the classes the logic distinguishes. The procedure is never read by
    the logic; the duration only matters relative to `0 ≤ d ≤ MAX_DURATION_US`.
  (Earlier values `HOLDER_CAPACITY = 3`, `FLUSHER_CAPACITY = 4` showed the
  same behavior with more states.)
- Recorded durations come from a small domain that includes invalid values,
  for example `{-1, 0, 5, 10, 11}`, so that flusher filtering is visible in traces.
- **Exhaustive verification uses tool-specific inputs.** Checking all states or
  all paths with the full model values above is not feasible (the state space
  is infinite because counters and `stored` grow without bound, and the number
  of paths grows exponentially with their length; see conclusions.md). Each
  tool therefore gets its own choice of inputs; the operations themselves are
  the same:
  - **TLC (all distinct states, any depth):** one procedure, durations
    `{5, 11}` (one valid, one invalid), at most 4 accepted samples (the
    minimum to overflow the flusher: fetch 2, collect 2 more, only 1 fits) and
    at most 1 sample dropped by a full holder. These bounds make the state
    space finite. They lose no behavior: the logic never branches on the value
    of a counter, only on capacities, which the bounds exceed.
  - **Apalache (all paths up to a given length):** any integer duration (the
    solver itself finds the values that matter, e.g. the boundaries of
    `0 ≤ d ≤ MAX_DURATION_US`), both procedures, length bounded by the number
    of steps.
  - The boundaries of the validity rule (-1, 0, 10, 11) are also checked by a
    dedicated scenario test, because TLC uses only `{5, 11}`.
- `SetRepositoryAvailable` is chosen nondeterministically so that write
  failures and retries appear in traces.
- The simulation step is biased towards operations that can succeed, so that
  interesting situations are reached often (coverage): most choices are
  guarded by the state in which the operation can succeed, some operations
  are weighted (e.g. `StoreStatsRecord` chosen more often so that the holder
  and the flusher fill up), and a share of choices is any operation in any
  state, so rejected calls still appear in traces. This only selects test
  inputs; it does not restrict the operations themselves.
- Coverage is measured, not assumed: every witness must be reached in a
  non-negligible share of simulated traces.
- Trace length is bounded by the checker's step limit (for example
  `--max-steps=30`), not by a variable in the spec.
- The spec may label the three `StoreStatsRecord` branches (accepted,
  ignored, dropped) so the MBT harness can report which one was expected. The
  harness infers the implementation's branch from the state change:
  `collected` grew, `dropped[c]` grew, or nothing changed.
- `lastResult`: verification-only variable recording the operation taken in
  the last step and its result (success/failure, `n` for `Fetch`). Used by the
  MBT harness for BG-01 and by the witnesses below.
- Ghost variables: spec variables prefixed `ghost` exist only for
  verification. They have no counterpart in the implementation and are not
  compared by the MBT harness. Current ghost variables (history flags, set
  from `lastResult`):
  - `ghostFlushFailedWhileRunning`: a `Flush` has failed while `Running` (W-02).
  - `ghostFlushedWhileStopping`: a `Flush` has succeeded while `Stopping` (W-06).
  - `ghostOverflowWhileRepoDown`: in some step `overflowed[c]` grew for some
    `c` while `repoAvailable = false` (W-07).

### Witnesses worth generating:
Scenarios the spec must be able to reach. Each condition below is stated in
positive form (true once the scenario has happened). The simulator checks
whether, and in how many traces, each scenario is reached (coverage). A trace
leading to a scenario can be produced on demand; it can also be replayed as a
targeted MBT test.
- **W-01 HolderFull**: a sample is dropped because the holder is full
  (`dropped[c] > 0` for some `c`).
- **W-02 FlushRetried**: a `Flush` returns failure while `Running` because the
  repository is unavailable (samples stay in processing); a later `Flush`,
  after the repository is back, returns success and stores those samples
  (`ghostFlushFailedWhileRunning` and `lastResult` is a successful `Flush`).
- **W-03 InvalidDiscarded**: an invalid sample is discarded by the flusher
  (`discarded[c] > 0` for some `c`).
- **W-04 StopWithRepositoryDown**: samples are lost during shutdown and
  `Stopped` is still reached (`lost[c] > 0` for some `c` and
  `state = Stopped`).
- **W-05 FullCycle**: a complete `Start → StoreStatsRecord → Stop → … →
  Finish` cycle with at least one sample stored (`state = Stopped` and
  `length(stored) > 0`).
- **W-06 StopDrainsToRepository**: samples still collected or in processing
  when `Stop` is called are stored in the repository during `Stopping`
  (repository available), and `Stopped` is reached with nothing lost
  (`ghostFlushedWhileStopping`, `state = Stopped` and `lost[c] = 0` for every `c`).
- **W-07 FlusherFull**: samples are overflowed at `Fetch` because processing
  is full during a repository outage (`ghostOverflowWhileRepoDown`).
  `overflowed[c] > 0` alone is not enough: if new samples are collected between
  two `Fetch` steps and no `Flush` runs in between, `processing` still holds
  the earlier samples and can overflow even with a working repository; and
  `overflowed` is a counter that does not show when the overflow happened.

## Phase 2: Multi-threading (out of scope for this version)
Not part of the current spec or implementation. Recorded here so the current
design does not block it.
- StatsOrchestrator works on a separate thread, running the `tick()` loop
  itself instead of being driven by the application.
- Data can be collected from various threads: collectors call
  `StoreStatsRecord` from any thread.
- Each operation must be atomic with respect to the others (e.g. the
  StatsHolder's `collected` sequence is guarded by a mutex). `Fetch`, `Flush`
  and `Finish` run only on the orchestrator thread.
- `Stop` must wait until the orchestrator thread has completed `Finish`.
- The spec, invariants and behavioral guarantees stay unchanged. Thread safety
  is not covered by model-based testing and needs separate concurrency tests
  (stress tests with many collector threads, ThreadSanitizer).
