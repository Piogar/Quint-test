#include "StatsHolder.h"

#include <utility>

namespace stats {

StatsHolder::StatsHolder(std::size_t capacity) : capacity_(capacity) {}

StoreOutcome StatsHolder::store(const StatRecord& record) {
    // Operation: StoreStatsRecord — Rejected (ignored) when state != Running.
    if (!accepting_) {
        return StoreOutcome::Ignored;
    }
    // Operation: StoreStatsRecord — Rejected (dropped) when the holder is full:
    // dropped[c] += 1, nothing else changes.
    if (collected_.size() >= capacity_) {
        ++dropped_[record.collector];
        return StoreOutcome::Dropped;
    }
    // Operation: StoreStatsRecord — Succeeds (accepted): append to `collected`.
    collected_.push_back(record);
    return StoreOutcome::Accepted;
}

std::vector<StatRecord> StatsHolder::takeAll() {
    std::vector<StatRecord> all;
    all.swap(collected_);
    return all;
}

void StatsHolder::setAccepting(bool accepting) { accepting_ = accepting; }

std::size_t StatsHolder::collectedCount() const { return collected_.size(); }

int64_t StatsHolder::dropped(const CollectorId& collector) const {
    auto it = dropped_.find(collector);
    return it == dropped_.end() ? 0 : it->second;
}

}  // namespace stats
