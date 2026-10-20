#pragma once

// StatsHolder: holds the collected samples (design doc state `collected`) and
// counts samples rejected because it was full (`dropped`).

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "StatsTypes.h"

namespace stats {

class StatsTestAccess;  // test-only access to private data (defined in tests/)

// Outcome of storing one sample in the holder. Internal to the framework: the
// collector uses it to decide whether to increment its sequence counter; it
// is not returned to the caller of StatsCollector::storeStatsRecord().
enum class StoreOutcome { Accepted, Ignored, Dropped };

class StatsHolder {
public:
    explicit StatsHolder(std::size_t capacity);

    // Operation: StoreStatsRecord (holder part).
    StoreOutcome store(const StatRecord& record);

    // Operation: Fetch (holder part): removes and returns all collected
    // samples, oldest first. The holder is always left empty.
    std::vector<StatRecord> takeAll();

    // Internal wiring, called only by StatsOrchestrator: start() -> true,
    // stop() -> false. "Accepting" is equivalent to state = Running (design
    // doc: "Who checks that the orchestrator is running").
    void setAccepting(bool accepting);

    // Query: CollectedCount.
    std::size_t collectedCount() const;

    // Query: Dropped(c). 0 for a collector that never had a sample dropped.
    int64_t dropped(const CollectorId& collector) const;

private:
    friend class StatsTestAccess;

    std::size_t capacity_;                     // HOLDER_CAPACITY
    bool accepting_ = false;                   // initially Stopped
    std::vector<StatRecord> collected_;        // state variable `collected`
    std::map<CollectorId, int64_t> dropped_;   // state variable `dropped`
};

}  // namespace stats
