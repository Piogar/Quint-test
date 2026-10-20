#include "StatsCollector.h"

#include <utility>

namespace stats {

StatsCollector::StatsCollector(CollectorId id, StatsHolder& holder) : id_(std::move(id)), holder_(holder) {}

void StatsCollector::storeStatsRecord(const ProcedureId& procedure, int64_t durationUs) {
    // Operation: StoreStatsRecord — the sample gets seq = nextSeq[c]; nextSeq[c]
    // is incremented only when the sample is accepted (not when it is ignored
    // or dropped).
    const StoreOutcome outcome = holder_.store(StatRecord{id_, procedure, nextSeq_, durationUs});
    if (outcome == StoreOutcome::Accepted) {
        ++nextSeq_;
    }
}

}  // namespace stats
