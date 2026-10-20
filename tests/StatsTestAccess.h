#pragma once

// Test-only read access to private data (design doc: "Visibility of
// accessors"). StatsHolder and StatsFlusher declare this class as a friend.

#include <vector>

#include "StatsFlusher.h"
#include "StatsHolder.h"
#include "StatsTypes.h"

namespace stats {

class StatsTestAccess {
public:
    // State variable `collected` (read-only copy).
    static std::vector<StatRecord> collected(const StatsHolder& holder) { return holder.collected_; }

    // State variable `processing` (read-only copy).
    static std::vector<StatRecord> processing(const StatsFlusher& flusher) { return flusher.processing_; }
};

}  // namespace stats
