#pragma once

// StatsFlusher: takes samples from the holder (Fetch), converts them and
// writes them to the repository (Flush). Holds the samples being processed
// (design doc state `processing`) and the counters `overflowed`, `discarded`
// and `lost`.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "IStatsRepository.h"
#include "StatsHolder.h"
#include "StatsTypes.h"

namespace stats {

class StatsTestAccess;  // test-only access to private data (defined in tests/)

class StatsFlusher {
public:
    StatsFlusher(StatsHolder& holder, IStatsRepository& repository, const StatsConfig& config);

    // Operation: Fetch. Returns std::nullopt when rejected, otherwise the
    // number of samples moved to processing (may be 0 if processing was full).
    std::optional<std::size_t> fetch();

    // Operation: Flush. Returns true on success, false when rejected.
    bool flush();

    // Flusher Conversion Rules: the valid samples of `batch`, unchanged and in
    // order. Pure: changes nothing.
    std::vector<StatRecord> convert(const std::vector<StatRecord>& batch) const;

    // Internal wiring, called only by StatsOrchestrator whenever its state
    // changes. Fetch and Flush depend on the orchestrator state (Fetch is
    // rejected when Stopped; a failed write loses the samples only while
    // Stopping). The orchestrator drives this state, so the flusher does not
    // depend on the orchestrator (same pattern as StatsHolder::setAccepting).
    void setOrchestratorState(OrchestratorState state);

    // Query: ProcessingCount.
    std::size_t processingCount() const;

    // Queries: Overflowed(c), Discarded(c), Lost(c). 0 for an unknown collector.
    int64_t overflowed(const CollectorId& collector) const;
    int64_t discarded(const CollectorId& collector) const;
    int64_t lost(const CollectorId& collector) const;

private:
    friend class StatsTestAccess;

    bool isValid(const StatRecord& record) const;
    static int64_t countOf(const std::map<CollectorId, int64_t>& counters, const CollectorId& collector);

    StatsHolder& holder_;
    IStatsRepository& repository_;
    std::size_t capacity_;                       // FLUSHER_CAPACITY
    int64_t maxDurationUs_;                      // MAX_DURATION_US
    OrchestratorState state_ = OrchestratorState::Stopped;

    std::vector<StatRecord> processing_;         // state variable `processing`
    std::map<CollectorId, int64_t> overflowed_;  // state variable `overflowed`
    std::map<CollectorId, int64_t> discarded_;   // state variable `discarded`
    std::map<CollectorId, int64_t> lost_;        // state variable `lost`
};

}  // namespace stats
