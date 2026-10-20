#pragma once

// Shared types of the stats framework.
// Design doc: docs/statsDESIGN.md, "Types" and "Spec to Implementation Mapping (C++)".

#include <cstddef>
#include <cstdint>
#include <string>

namespace stats {

// Identifier of a StatsCollector instance (design doc: CollectorId).
using CollectorId = std::string;

// Identifier of a measured procedure (design doc: ProcedureId).
using ProcedureId = std::string;

// One performance sample (design doc: StatRecord). The same type is used in
// all three stages: collected (holder), processing (flusher), stored
// (repository).
struct StatRecord {
    CollectorId collector;
    ProcedureId procedure;
    int64_t seq = 0;          // per-collector sequence number assigned at StoreStatsRecord
    int64_t durationUs = 0;

    bool operator==(const StatRecord& other) const {
        return collector == other.collector && procedure == other.procedure &&
               seq == other.seq && durationUs == other.durationUs;
    }
    bool operator!=(const StatRecord& other) const { return !(*this == other); }
};

// Lifecycle of the orchestrator (design doc: OrchestratorState).
enum class OrchestratorState { Stopped, Running, Stopping };

// Configuration fixed when the framework is created (design doc: Constants).
struct StatsConfig {
    std::size_t holderCapacity = 0;   // HOLDER_CAPACITY (> 0)
    std::size_t flusherCapacity = 0;  // FLUSHER_CAPACITY (> 0)
    int64_t maxDurationUs = 0;        // MAX_DURATION_US (>= 0)
};

}  // namespace stats
