#pragma once

// StatsOrchestrator: lifecycle of the framework (design doc state `state`).
// Drives the holder (accepting samples) and the flusher (its view of the
// orchestrator state); neither of them depends on the orchestrator.

#include "StatsFlusher.h"
#include "StatsHolder.h"
#include "StatsTypes.h"

namespace stats {

class StatsOrchestrator {
public:
    StatsOrchestrator(StatsHolder& holder, StatsFlusher& flusher);

    // Operation: Start. true on success, false when rejected.
    bool start();

    // Operation: Stop. true on success, false when rejected.
    bool stop();

    // Operation: Finish. Public: the application may call it; tick() calls it
    // while stopping; MBT calls it directly. true on success, false when
    // rejected.
    bool finish();

    // Called periodically by the application: fetch(), flush() and, while
    // stopping, finish(). Composes the operations; adds no behavior of its own.
    void tick();

    // Query: State.
    OrchestratorState state() const { return state_; }

private:
    void setState(OrchestratorState state);

    StatsHolder& holder_;
    StatsFlusher& flusher_;
    OrchestratorState state_ = OrchestratorState::Stopped;
};

}  // namespace stats
