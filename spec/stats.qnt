// -*- mode: Bluespec; -*-

// Stats Framework — single-threaded model
// Based on: ../docs/statsDESIGN.md
//
// Modules in this file:
//   StatsFramework              – the spec: behavior the C++ code must implement
//                                 (parameterized by the design-doc constants)
//   StatsFrameworkVerification  – instance with small model values, nondeterministic
//                                 `step`, witnesses and scenario tests
//                                 (quint run / quint test / MBT)
//
// Naming rule: variables prefixed `ghost` exist only for verification. They
// have no counterpart in the implementation and are not compared by MBT.

//////////////////////////////////////////////////////////////////////////////
// Spec
//////////////////////////////////////////////////////////////////////////////

// Structure (thin actions): each operation is a pure function `Sys -> Transition`
// (`*Op`); the action `do*` reads the current state, calls it and writes the
// result back.
//
// Rejected operations are steps too (state unchanged, failure recorded in
// `lastResult`), so MBT traces also exercise rejections (BG-01, BG-04).
module StatsFramework {

  //////////////////////////////////////////////////////////////////////////
  // Constants (design doc: Constants)
  //////////////////////////////////////////////////////////////////////////

  const COLLECTORS: Set[str]      // COLLECTORS
  const PROCEDURES: Set[str]      // PROCEDURES
  const HOLDER_CAPACITY: int      // HOLDER_CAPACITY
  const FLUSHER_CAPACITY: int     // FLUSHER_CAPACITY
  const MAX_DURATION_US: int      // MAX_DURATION_US

  assume HolderCapacityPositive = HOLDER_CAPACITY > 0
  assume FlusherCapacityPositive = FLUSHER_CAPACITY > 0
  assume MaxDurationNonNegative = MAX_DURATION_US >= 0

  //////////////////////////////////////////////////////////////////////////
  // Types (design doc: Types)
  //////////////////////////////////////////////////////////////////////////

  type CollectorId = str
  type ProcedureId = str

  type StatRecord = {
    collector: CollectorId,
    procedure: ProcedureId,
    seq: int,         // per-collector sequence number assigned at StoreStatsRecord
    durationUs: int
  }

  type OrchestratorState = Stopped | Running | Stopping

  // Labels of the three StoreStatsRecord branches. Verification only:
  // StoreStatsRecord reports no result to its caller (design doc: Operations).
  type StoreOutcome = Accepted | Ignored | Dropped

  // Result of the last operation = what the caller got back (BG-01).
  type OpResult =
    | NoResult                                // after init; not an operation result
    | StartResult(bool)                       // Start: success / failure
    | StoreStatsRecordResult(StoreOutcome)    // StoreStatsRecord: verification label only
    | FetchResult({ ok: bool, n: int })       // Fetch: success with n / failure
    | FlushResult(bool)                       // Flush: success / failure
    | StopResult(bool)                        // Stop: success / failure
    | FinishResult(bool)                      // Finish: success / failure
    | SetRepositoryAvailableResult(bool)      // environment step, always succeeds

  // All model state (design doc: Abstract State)
  type Sys = {
    state: OrchestratorState,
    collected: List[StatRecord],
    processing: List[StatRecord],
    stored: List[StatRecord],
    nextSeq: CollectorId -> int,
    dropped: CollectorId -> int,
    overflowed: CollectorId -> int,
    discarded: CollectorId -> int,
    lost: CollectorId -> int,
    repoAvailable: bool
  }

  type Transition = { sys: Sys, result: OpResult }

  //////////////////////////////////////////////////////////////////////////
  // Pure helpers
  //////////////////////////////////////////////////////////////////////////

  pure def mkTr(s: Sys, r: OpResult): Transition = { sys: s, result: r }

  pure def minInt(a: int, b: int): int = if (a < b) a else b

  pure val zeroCounters: CollectorId -> int = COLLECTORS.mapBy(_ => 0)

  // Increment `counters[r.collector]` once for every sample `r` in `l`
  pure def countInto(counters: CollectorId -> int, l: List[StatRecord]): CollectorId -> int =
    l.foldl(counters, (acc, r) => acc.setBy(r.collector, n => n + 1))

  // Number of samples of collector `c` in `l`
  pure def countOf(l: List[StatRecord], c: CollectorId): int =
    l.foldl(0, (acc, r) => if (r.collector == c) acc + 1 else acc)

  //////////////////////////////////////////////////////////////////////////
  // Flusher Conversion Rules (design doc: Flusher Conversion Rules)
  //////////////////////////////////////////////////////////////////////////

  // A sample is valid when 0 ≤ durationUs ≤ MAX_DURATION_US
  pure def isValid(r: StatRecord): bool =
    0 <= r.durationUs and r.durationUs <= MAX_DURATION_US

  // convert(processing): valid samples, unchanged and in order
  pure def convert(l: List[StatRecord]): List[StatRecord] =
    l.select(r => isValid(r))

  //////////////////////////////////////////////////////////////////////////
  // Operations as pure functions (design doc: Operations)
  //////////////////////////////////////////////////////////////////////////

  // Initial state (design doc: Abstract State, "Initially ...")
  pure val initSys: Sys = {
    state: Stopped,
    collected: List(),
    processing: List(),
    stored: List(),
    nextSeq: zeroCounters,
    dropped: zeroCounters,
    overflowed: zeroCounters,
    discarded: zeroCounters,
    lost: zeroCounters,
    repoAvailable: true
  }

  // Operation: Start (caller: application)
  pure def startOp(s: Sys): Transition =
    if (s.state == Stopped)
      mkTr({ ...s, state: Running }, StartResult(true))
    else
      mkTr(s, StartResult(false))

  // Operation: StoreStatsRecord(c, p, d) (caller: StatsCollector c)
  pure def storeStatsRecordOp(s: Sys, c: CollectorId, p: ProcedureId, d: int): Transition =
    if (s.state != Running)
      // Rejected (ignored): not running
      mkTr(s, StoreStatsRecordResult(Ignored))
    else if (s.collected.length() >= HOLDER_CAPACITY)
      // Rejected (dropped): holder full
      mkTr({ ...s, dropped: s.dropped.setBy(c, n => n + 1) }, StoreStatsRecordResult(Dropped))
    else {
      // Succeeds (accepted)
      val r = { collector: c, procedure: p, seq: s.nextSeq.get(c), durationUs: d }
      mkTr(
        { ...s, collected: s.collected.append(r), nextSeq: s.nextSeq.setBy(c, n => n + 1) },
        StoreStatsRecordResult(Accepted)
      )
    }

  // Operation: Fetch (caller: StatsFlusher, triggered by the orchestrator)
  pure def fetchOp(s: Sys): Transition =
    if (s.state == Stopped or s.collected.length() == 0)
      mkTr(s, FetchResult({ ok: false, n: 0 }))
    else {
      // k = min(length(collected), FLUSHER_CAPACITY - length(processing))
      val k = minInt(s.collected.length(), FLUSHER_CAPACITY - s.processing.length())
      val moved = s.collected.slice(0, k)
      val rest = s.collected.slice(k, s.collected.length())
      mkTr(
        {
          ...s,
          processing: s.processing.concat(moved),
          collected: List(),
          overflowed: countInto(s.overflowed, rest)
        },
        FetchResult({ ok: true, n: k })
      )
    }

  // Operation: Flush (caller: StatsFlusher, triggered by the orchestrator)
  pure def flushOp(s: Sys): Transition =
    if (s.processing.length() == 0)
      // Rejected: nothing to flush
      mkTr(s, FlushResult(false))
    else if (s.repoAvailable) {
      // Succeeds: valid samples stored (all-or-nothing), invalid ones discarded
      val invalid = s.processing.select(r => not(isValid(r)))
      mkTr(
        {
          ...s,
          stored: s.stored.concat(convert(s.processing)),
          discarded: countInto(s.discarded, invalid),
          processing: List()
        },
        FlushResult(true)
      )
    }
    else if (s.state == Stopping)
      // Rejected while Stopping: samples removed and counted as lost (BG-05a)
      mkTr({ ...s, lost: countInto(s.lost, s.processing), processing: List() }, FlushResult(false))
    else
      // Rejected while Running: samples kept for a later retry (BG-05)
      mkTr(s, FlushResult(false))

  // Operation: Stop (caller: application)
  pure def stopOp(s: Sys): Transition =
    if (s.state == Running)
      mkTr({ ...s, state: Stopping }, StopResult(true))
    else
      mkTr(s, StopResult(false))

  // Operation: Finish (caller: StatsOrchestrator)
  pure def finishOp(s: Sys): Transition =
    if (s.state == Stopping and s.collected.length() == 0 and s.processing.length() == 0)
      mkTr({ ...s, state: Stopped }, FinishResult(true))
    else
      mkTr(s, FinishResult(false))

  // Operation: SetRepositoryAvailable(b) (caller: environment)
  pure def setRepositoryAvailableOp(s: Sys, b: bool): Transition =
    mkTr({ ...s, repoAvailable: b }, SetRepositoryAvailableResult(true))

  // Ghost flag updates (verification only). `pre` is the state before the
  // step, `r` the result of the step.
  pure def nextGhostFlushFailedWhileRunning(flag: bool, pre: Sys, r: OpResult): bool =
    flag or (r == FlushResult(false) and pre.state == Running and pre.processing.length() > 0)

  pure def nextGhostFlushedWhileStopping(flag: bool, pre: Sys, r: OpResult): bool =
    flag or (r == FlushResult(true) and pre.state == Stopping)

  // In this step `overflowed` grew for some collector AND the repository was
  // unavailable (`post` is the state after the step)
  pure def nextGhostOverflowWhileRepoDown(flag: bool, pre: Sys, post: Sys): bool =
    flag or (
      COLLECTORS.exists(c => post.overflowed.get(c) > pre.overflowed.get(c))
        and not(pre.repoAvailable)
    )

  //////////////////////////////////////////////////////////////////////////
  // State variables (design doc: Abstract State)
  //////////////////////////////////////////////////////////////////////////

  var state: OrchestratorState            // StatsOrchestrator
  var collected: List[StatRecord]         // StatsHolder
  var processing: List[StatRecord]        // StatsFlusher
  var stored: List[StatRecord]            // StatsRepository
  var nextSeq: CollectorId -> int         // StatsCollector
  var dropped: CollectorId -> int         // StatsHolder
  var overflowed: CollectorId -> int      // StatsFlusher
  var discarded: CollectorId -> int       // StatsFlusher
  var lost: CollectorId -> int            // StatsFlusher
  var repoAvailable: bool                 // environment
  var lastResult: OpResult                // result of the last operation (BG-01)

  // Ghost variables (verification only; not in the implementation, not compared by MBT)
  var ghostFlushFailedWhileRunning: bool  // a Flush has failed while Running (W-02)
  var ghostFlushedWhileStopping: bool     // a Flush has succeeded while Stopping (W-06)
  var ghostOverflowWhileRepoDown: bool    // samples overflowed while the repository was unavailable (W-07)

  val current: Sys = {
    state: state,
    collected: collected,
    processing: processing,
    stored: stored,
    nextSeq: nextSeq,
    dropped: dropped,
    overflowed: overflowed,
    discarded: discarded,
    lost: lost,
    repoAvailable: repoAvailable
  }

  //////////////////////////////////////////////////////////////////////////
  // Actions (thin wrappers, one per operation)
  //////////////////////////////////////////////////////////////////////////

  // Writes every state variable; variables an operation does not modify are
  // carried over unchanged inside `t.sys`.
  action applyTransition(t: Transition): bool = all {
    state' = t.sys.state,
    collected' = t.sys.collected,
    processing' = t.sys.processing,
    stored' = t.sys.stored,
    nextSeq' = t.sys.nextSeq,
    dropped' = t.sys.dropped,
    overflowed' = t.sys.overflowed,
    discarded' = t.sys.discarded,
    lost' = t.sys.lost,
    repoAvailable' = t.sys.repoAvailable,
    lastResult' = t.result,
    ghostFlushFailedWhileRunning' =
      nextGhostFlushFailedWhileRunning(ghostFlushFailedWhileRunning, current, t.result),
    ghostFlushedWhileStopping' =
      nextGhostFlushedWhileStopping(ghostFlushedWhileStopping, current, t.result),
    ghostOverflowWhileRepoDown' =
      nextGhostOverflowWhileRepoDown(ghostOverflowWhileRepoDown, current, t.sys),
  }

  action specInit = all {
    state' = initSys.state,
    collected' = initSys.collected,
    processing' = initSys.processing,
    stored' = initSys.stored,
    nextSeq' = initSys.nextSeq,
    dropped' = initSys.dropped,
    overflowed' = initSys.overflowed,
    discarded' = initSys.discarded,
    lost' = initSys.lost,
    repoAvailable' = initSys.repoAvailable,
    lastResult' = NoResult,
    ghostFlushFailedWhileRunning' = false,
    ghostFlushedWhileStopping' = false,
    ghostOverflowWhileRepoDown' = false,
  }

  action doStart = applyTransition(startOp(current))

  action doStoreStatsRecord(c: CollectorId, p: ProcedureId, d: int): bool =
    applyTransition(storeStatsRecordOp(current, c, p, d))

  action doFetch = applyTransition(fetchOp(current))

  action doFlush = applyTransition(flushOp(current))

  action doStop = applyTransition(stopOp(current))

  action doFinish = applyTransition(finishOp(current))

  action doSetRepositoryAvailable(b: bool): bool =
    applyTransition(setRepositoryAvailableOp(current, b))

  //////////////////////////////////////////////////////////////////////////
  // Queries (design doc: Queries)
  //////////////////////////////////////////////////////////////////////////

  val collectedCount: int = collected.length()     // CollectedCount
  val processingCount: int = processing.length()   // ProcessingCount
  val storedCount: int = stored.length()           // StoredCount

  //////////////////////////////////////////////////////////////////////////
  // State invariants (design doc: State Invariants)
  //////////////////////////////////////////////////////////////////////////

  // INV-01 BoundedCollected
  val inv01BoundedCollected: bool =
    collected.length() <= HOLDER_CAPACITY

  // INV-02 BoundedProcessing
  val inv02BoundedProcessing: bool =
    processing.length() <= FLUSHER_CAPACITY

  // INV-03 Conservation
  val inv03Conservation: bool = COLLECTORS.forall(c =>
    nextSeq.get(c) == countOf(collected, c) + countOf(processing, c) + countOf(stored, c)
      + overflowed.get(c) + discarded.get(c) + lost.get(c)
  )

  // Oldest first across stages: stored, then processing, then collected
  val allSamples: List[StatRecord] = stored.concat(processing).concat(collected)

  // INV-04 UniqueSamples
  val inv04UniqueSamples: bool =
    allSamples.indices().forall(i => allSamples.indices().forall(j =>
      i == j
        or allSamples[i].collector != allSamples[j].collector
        or allSamples[i].seq != allSamples[j].seq
    ))

  // INV-05 PerCollectorOrder: strictly increasing seq per collector within
  // each stage and across stages (stored < processing < collected), i.e.
  // within the concatenation above
  val inv05PerCollectorOrder: bool =
    allSamples.indices().forall(i => allSamples.indices().forall(j =>
      (i < j and allSamples[i].collector == allSamples[j].collector)
        implies allSamples[i].seq < allSamples[j].seq
    ))

  // INV-06 OnlyValidStored
  val inv06OnlyValidStored: bool =
    stored.foldl(true, (ok, r) => ok and isValid(r))

  // INV-07 StoppedIsDrained
  val inv07StoppedIsDrained: bool =
    state == Stopped implies (collected.length() == 0 and processing.length() == 0)

  // INV-08 NonNegativeCounters
  val inv08NonNegativeCounters: bool = COLLECTORS.forall(c => and {
    nextSeq.get(c) >= 0,
    dropped.get(c) >= 0,
    overflowed.get(c) >= 0,
    discarded.get(c) >= 0,
    lost.get(c) >= 0,
  })

  // All state invariants INV-01 … INV-08
  val safetyInvariant: bool = and {
    inv01BoundedCollected,
    inv02BoundedProcessing,
    inv03Conservation,
    inv04UniqueSamples,
    inv05PerCollectorOrder,
    inv06OnlyValidStored,
    inv07StoppedIsDrained,
    inv08NonNegativeCounters,
  }

  //////////////////////////////////////////////////////////////////////////
  // Temporal property (design doc: Temporal property)
  //////////////////////////////////////////////////////////////////////////

  // TP-01 StopTerminates, stated as a state property: from any Stopping
  // state, Fetch, Flush, Finish with no other operation in between reach
  // Stopped, whatever the repository availability.
  val tp01StopTerminates: bool =
    state == Stopping implies finishOp(flushOp(fetchOp(current).sys).sys).sys.state == Stopped
}

//////////////////////////////////////////////////////////////////////////////
// Verification only
//////////////////////////////////////////////////////////////////////////////

// Instance with the small model values (design doc: Verification Only),
// nondeterministic `step`, witnesses and scenario tests.
//   quint run  stats.qnt --main=StatsFrameworkVerification --invariant=allProperties
//   quint test stats.qnt --main=StatsFrameworkVerification
module StatsFrameworkVerification {

  // Model constants (design doc: Verification Only, "Model constants")
  pure val MODEL_COLLECTORS: Set[str] = Set("c1", "c2")
  pure val MODEL_PROCEDURES: Set[str] = Set("p1", "p2")
  // Durations a collector may report, including invalid ones
  pure val MODEL_DURATIONS: Set[int] = Set(-1, 0, 5, 10, 11)

  import StatsFramework(
    COLLECTORS = MODEL_COLLECTORS,
    PROCEDURES = MODEL_PROCEDURES,
    HOLDER_CAPACITY = 3,
    FLUSHER_CAPACITY = 4,
    MAX_DURATION_US = 10
  ).*

  //////////////////////////////////////////////////////////////////////////
  // Simulation: any operation, in any order, with nondeterministic arguments
  //////////////////////////////////////////////////////////////////////////

  action init = specInit

  action storeAnySample = {
    nondet c = MODEL_COLLECTORS.oneOf()
    nondet p = MODEL_PROCEDURES.oneOf()
    nondet d = MODEL_DURATIONS.oneOf()
    doStoreStatsRecord(c, p, d)
  }

  action setAnyRepositoryAvailability = {
    nondet b = Set(true, false).oneOf()
    doSetRepositoryAvailable(b)
  }

  // Any operation, whether or not it can succeed. Keeps rejected calls in
  // traces, so MBT still checks rejections (BG-01, BG-04).
  action anyCall = any {
    doStart,
    storeAnySample,
    doFetch,
    doFlush,
    doStop,
    doFinish,
    setAnyRepositoryAvailability,
  }

  // Simulation step. The simulator picks uniformly among the branches whose
  // guard holds in the current state, so:
  // - guards make most steps operations that can succeed (better coverage;
  //   see conclusions.md, "Beware of low coverage");
  // - a branch listed several times is picked proportionally more often
  //   (weight); StoreStatsRecord is listed twice so the holder and the
  //   flusher fill up more often;
  // - `anyCall` keeps a share of unrestricted (possibly rejected) calls.
  // Guards here only choose test inputs; the spec actions are unchanged.
  action step = any {
    all { state == Stopped, doStart },
    all { state == Running, storeAnySample },
    all { state == Running, storeAnySample },
    all { state != Stopped, collected.length() > 0, doFetch },
    all { processing.length() > 0, doFlush },
    all { state == Running, doStop },
    all { state == Stopping, collected.length() == 0, processing.length() == 0, doFinish },
    setAnyRepositoryAvailability,
    anyCall,
  }

  // Everything checked on every state: INV-01 … INV-08 and TP-01
  val allProperties: bool = safetyInvariant and tp01StopTerminates

  //////////////////////////////////////////////////////////////////////////
  // Witnesses (design doc: Witnesses worth generating)
  //
  // Each is the scenario in POSITIVE form: true in a state where the scenario
  // has happened.
  // - Is it reached, and how often? (coverage)
  //     quint run ... --witnesses w01HolderFull ...
  //   reports in how many traces each one became true (N > 0 = reached).
  // - How was it reached? (trace, e.g. a targeted MBT case)
  //     quint run ... --invariant='not(w07FlusherFull)' --mbt
  //   the violation trace is the witness.
  //////////////////////////////////////////////////////////////////////////

  // W-01 HolderFull
  val w01HolderFull: bool =
    MODEL_COLLECTORS.exists(c => dropped.get(c) > 0)

  // W-02 FlushRetried
  val w02FlushRetried: bool =
    ghostFlushFailedWhileRunning and lastResult == FlushResult(true)

  // W-03 InvalidDiscarded
  val w03InvalidDiscarded: bool =
    MODEL_COLLECTORS.exists(c => discarded.get(c) > 0)

  // W-04 StopWithRepositoryDown
  val w04StopWithRepositoryDown: bool =
    MODEL_COLLECTORS.exists(c => lost.get(c) > 0) and state == Stopped

  // W-05 FullCycle
  val w05FullCycle: bool =
    state == Stopped and stored.length() > 0

  // W-06 StopDrainsToRepository
  val w06StopDrainsToRepository: bool =
    ghostFlushedWhileStopping and state == Stopped and MODEL_COLLECTORS.forall(c => lost.get(c) == 0)

  // W-07 FlusherFull: samples overflowed at Fetch while the repository was
  // unavailable (not merely any overflow: with new samples collected between
  // two Fetches and no Flush in between, processing can overflow even with a
  // working repository)
  val w07FlusherFull: bool =
    ghostOverflowWhileRepoDown

  //////////////////////////////////////////////////////////////////////////
  // Scenario tests (one per witness, plus rejections)
  //////////////////////////////////////////////////////////////////////////

  // W-05 FullCycle: Start -> StoreStatsRecord -> Fetch -> Flush -> Stop -> Finish
  run fullCycleTest =
    specInit
      .then(doStart)
      .then(doStoreStatsRecord("c1", "p1", 5))
      .then(doFetch)
      .expect(lastResult == FetchResult({ ok: true, n: 1 }) and processing.length() == 1)
      .then(doFlush)
      .expect(lastResult == FlushResult(true))
      .then(doStop)
      .then(doFinish)
      .expect(and {
        state == Stopped,
        stored == List({ collector: "c1", procedure: "p1", seq: 0, durationUs: 5 }),
        allProperties,
      })

  // W-01 HolderFull: fourth sample is dropped by the full holder (capacity 3)
  run holderFullTest =
    specInit
      .then(doStart)
      .then(doStoreStatsRecord("c1", "p1", 0))
      .then(doStoreStatsRecord("c1", "p1", 0))
      .then(doStoreStatsRecord("c2", "p2", 0))
      .then(doStoreStatsRecord("c2", "p2", 0))
      .expect(and {
        lastResult == StoreStatsRecordResult(Dropped),
        collected.length() == 3,
        dropped.get("c2") == 1,
        nextSeq.get("c2") == 1,
        allProperties,
      })

  // W-02 FlushRetried / BG-05: failed Flush while Running keeps samples;
  // later fetches append behind them; the retry stores them first, in order
  run flushRetriedTest =
    specInit
      .then(doStart)
      .then(doSetRepositoryAvailable(false))
      .then(doStoreStatsRecord("c1", "p1", 5))
      .then(doFetch)
      .then(doFlush)
      .expect(lastResult == FlushResult(false) and processing.length() == 1)
      .then(doStoreStatsRecord("c1", "p2", 0))
      .then(doFetch)
      .expect(processing.length() == 2 and processing[0].seq == 0 and processing[1].seq == 1)
      .then(doSetRepositoryAvailable(true))
      .then(doFlush)
      .expect(and {
        lastResult == FlushResult(true),
        stored.length() == 2,
        stored[0].seq == 0,
        allProperties,
      })

  // W-03 InvalidDiscarded: invalid durations are discarded, not stored
  run invalidDiscardedTest =
    specInit
      .then(doStart)
      .then(doStoreStatsRecord("c1", "p1", -1))
      .then(doStoreStatsRecord("c1", "p1", 11))
      .then(doStoreStatsRecord("c2", "p1", 10))
      .then(doFetch)
      .then(doFlush)
      .expect(and {
        stored.length() == 1,
        discarded.get("c1") == 2,
        allProperties,
      })

  // W-04 StopWithRepositoryDown / BG-05a: samples lost, Stopped still reached
  run stopWithRepositoryDownTest =
    specInit
      .then(doStart)
      .then(doStoreStatsRecord("c1", "p1", 5))
      .then(doSetRepositoryAvailable(false))
      .then(doStop)
      .then(doStoreStatsRecord("c2", "p1", 5))
      .expect(lastResult == StoreStatsRecordResult(Ignored) and collected.length() == 1)
      .then(doFetch)
      .then(doFlush)
      .expect(lastResult == FlushResult(false) and processing.length() == 0 and lost.get("c1") == 1)
      .then(doFinish)
      .expect(and {
        state == Stopped,
        stored.length() == 0,
        allProperties,
      })

  // W-06 StopDrainsToRepository: samples pending at Stop are stored during Stopping
  run stopDrainsToRepositoryTest =
    specInit
      .then(doStart)
      .then(doStoreStatsRecord("c1", "p1", 5))
      .then(doStoreStatsRecord("c2", "p2", 5))
      .then(doStop)
      .then(doFinish)
      .expect(lastResult == FinishResult(false))
      .then(doFetch)
      .then(doFlush)
      .expect(lastResult == FlushResult(true))
      .then(doFinish)
      .expect(and {
        state == Stopped,
        stored.length() == 2,
        MODEL_COLLECTORS.forall(c => lost.get(c) == 0),
        allProperties,
      })

  // W-07 FlusherFull: repository outage fills processing (capacity 4); extra samples overflow
  run flusherFullTest =
    specInit
      .then(doStart)
      .then(doSetRepositoryAvailable(false))
      .then(doStoreStatsRecord("c1", "p1", 5))
      .then(doStoreStatsRecord("c1", "p1", 5))
      .then(doStoreStatsRecord("c1", "p1", 5))
      .then(doFetch)
      .then(doFlush)
      .then(doStoreStatsRecord("c2", "p1", 5))
      .then(doStoreStatsRecord("c2", "p1", 5))
      .then(doStoreStatsRecord("c2", "p1", 5))
      .then(doFetch)
      .expect(and {
        lastResult == FetchResult({ ok: true, n: 1 }),
        processing.length() == 4,
        collected.length() == 0,
        overflowed.get("c2") == 2,
        allProperties,
      })

  // BG-04 Rejection: rejected operations report failure and change nothing
  run rejectionsTest =
    specInit
      .then(doFetch)
      .expect(lastResult == FetchResult({ ok: false, n: 0 }))
      .then(doFlush)
      .expect(lastResult == FlushResult(false))
      .then(doStop)
      .expect(lastResult == StopResult(false))
      .then(doFinish)
      .expect(lastResult == FinishResult(false))
      .then(doStoreStatsRecord("c1", "p1", 5))
      .expect(lastResult == StoreStatsRecordResult(Ignored) and nextSeq.get("c1") == 0)
      .then(doStart)
      .then(doStart)
      .expect(lastResult == StartResult(false) and state == Running and allProperties)
}
