#include "StatsFlusher.h"

#include <algorithm>

namespace stats {

StatsFlusher::StatsFlusher(StatsHolder& holder, IStatsRepository& repository, const StatsConfig& config)
    : holder_(holder),
      repository_(repository),
      capacity_(config.flusherCapacity),
      maxDurationUs_(config.maxDurationUs) {}

std::optional<std::size_t> StatsFlusher::fetch() {
    // Operation: Fetch — Rejected when state = Stopped, or collected is empty.
    if (state_ == OrchestratorState::Stopped || holder_.collectedCount() == 0) {
        return std::nullopt;
    }
    // Operation: Fetch — Succeeds: k = min(length(collected),
    // FLUSHER_CAPACITY - length(processing)); the first k samples are appended
    // behind the samples still in processing; every remaining sample is
    // counted in overflowed[collector]; the holder is left empty.
    std::vector<StatRecord> taken = holder_.takeAll();
    const std::size_t room = capacity_ > processing_.size() ? capacity_ - processing_.size() : 0;
    const std::size_t k = std::min(taken.size(), room);
    for (std::size_t i = 0; i < taken.size(); ++i) {
        if (i < k) {
            processing_.push_back(taken[i]);
        } else {
            ++overflowed_[taken[i].collector];
        }
    }
    return k;
}

bool StatsFlusher::flush() {
    // Operation: Flush — Rejected when processing is empty (nothing to flush).
    if (processing_.empty()) {
        return false;
    }
    // Operation: Flush — Succeeds when the repository is available: the valid
    // samples are written (all-or-nothing, in order), every invalid sample is
    // counted in discarded[collector], processing becomes empty.
    if (repository_.write(convert(processing_))) {
        for (const StatRecord& record : processing_) {
            if (!isValid(record)) {
                ++discarded_[record.collector];
            }
        }
        processing_.clear();
        return true;
    }
    // Operation: Flush — Rejected while Stopping (repository unavailable): every
    // sample is counted in lost[collector], processing becomes empty.
    if (state_ == OrchestratorState::Stopping) {
        for (const StatRecord& record : processing_) {
            ++lost_[record.collector];
        }
        processing_.clear();
    }
    // Operation: Flush — Rejected while Running (repository unavailable):
    // processing is kept for a later retry.
    return false;
}

std::vector<StatRecord> StatsFlusher::convert(const std::vector<StatRecord>& batch) const {
    std::vector<StatRecord> valid;
    for (const StatRecord& record : batch) {
        if (isValid(record)) {
            valid.push_back(record);
        }
    }
    return valid;
}

void StatsFlusher::setOrchestratorState(OrchestratorState state) { state_ = state; }

std::size_t StatsFlusher::processingCount() const { return processing_.size(); }

int64_t StatsFlusher::overflowed(const CollectorId& collector) const { return countOf(overflowed_, collector); }
int64_t StatsFlusher::discarded(const CollectorId& collector) const { return countOf(discarded_, collector); }
int64_t StatsFlusher::lost(const CollectorId& collector) const { return countOf(lost_, collector); }

// Flusher Conversion Rules: a sample is valid when 0 <= durationUs <= MAX_DURATION_US.
bool StatsFlusher::isValid(const StatRecord& record) const {
    return record.durationUs >= 0 && record.durationUs <= maxDurationUs_;
}

int64_t StatsFlusher::countOf(const std::map<CollectorId, int64_t>& counters, const CollectorId& collector) {
    auto it = counters.find(collector);
    return it == counters.end() ? 0 : it->second;
}

}  // namespace stats
