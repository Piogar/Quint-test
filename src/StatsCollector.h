#pragma once

// StatsCollector: entry point used across the source code to gather single
// performance samples. Depends only on StatsHolder, never on the orchestrator
// (design doc: "Who checks that the orchestrator is running").

#include <cstdint>

#include "StatsHolder.h"
#include "StatsTypes.h"

namespace stats {

class StatsCollector {
public:
    StatsCollector(CollectorId id, StatsHolder& holder);

    // Operation: StoreStatsRecord(c, p, d). Fire-and-forget: reports nothing
    // to the caller; a sample may be ignored (not running) or dropped (holder
    // full) without the caller knowing.
    void storeStatsRecord(const ProcedureId& procedure, int64_t durationUs);

    const CollectorId& id() const { return id_; }

private:
    CollectorId id_;
    StatsHolder& holder_;
    int64_t nextSeq_ = 0;  // state variable `nextSeq[c]`
};

}  // namespace stats
