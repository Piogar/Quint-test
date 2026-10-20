#pragma once

// In-memory fake repository (test only). Design doc mapping: `stored` and
// `repoAvailable`; SetRepositoryAvailable(b) -> setAvailable(b).

#include <vector>

#include "IStatsRepository.h"
#include "StatsTypes.h"

namespace stats {

class InMemoryStatsRepository : public IStatsRepository {
public:
    bool write(const std::vector<StatRecord>& records) override {
        if (!available_) {
            return false;  // unavailable: nothing is written (all-or-nothing)
        }
        stored_.insert(stored_.end(), records.begin(), records.end());
        return true;
    }

    // SetRepositoryAvailable(b) (environment, test only)
    void setAvailable(bool available) { available_ = available; }
    bool available() const { return available_; }

    // State variable `stored`, in storage order.
    const std::vector<StatRecord>& stored() const { return stored_; }

private:
    bool available_ = true;  // initially available
    std::vector<StatRecord> stored_;
};

}  // namespace stats
