#pragma once

// Target data repository (e.g. a DB adapter in production, an in-memory fake
// in tests). Design doc: "Spec to Implementation Mapping (C++)", rows
// `stored` and `repoAvailable`.

#include <vector>

#include "StatsTypes.h"

namespace stats {

class IStatsRepository {
public:
    virtual ~IStatsRepository() = default;

    // Writes all records, in order, or none of them (all-or-nothing).
    // Returns false when the repository is unavailable; nothing is written
    // then. An empty batch is written successfully only if the repository is
    // available.
    virtual bool write(const std::vector<StatRecord>& records) = 0;
};

}  // namespace stats
