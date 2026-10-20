#include "StatsOrchestrator.h"

namespace stats {

StatsOrchestrator::StatsOrchestrator(StatsHolder& holder, StatsFlusher& flusher)
    : holder_(holder), flusher_(flusher) {
    setState(OrchestratorState::Stopped);
}

bool StatsOrchestrator::start() {
    // Operation: Start — Rejected when state != Stopped.
    if (state_ != OrchestratorState::Stopped) {
        return false;
    }
    // Operation: Start — Succeeds: state := Running.
    setState(OrchestratorState::Running);
    return true;
}

bool StatsOrchestrator::stop() {
    // Operation: Stop — Rejected when state != Running.
    if (state_ != OrchestratorState::Running) {
        return false;
    }
    // Operation: Stop — Succeeds: state := Stopping (new samples are ignored,
    // samples already collected or in processing are still drained).
    setState(OrchestratorState::Stopping);
    return true;
}

bool StatsOrchestrator::finish() {
    // Operation: Finish — Succeeds when state = Stopping, collected is empty
    // and processing is empty: state := Stopped.
    if (state_ == OrchestratorState::Stopping && holder_.collectedCount() == 0 &&
        flusher_.processingCount() == 0) {
        setState(OrchestratorState::Stopped);
        return true;
    }
    // Operation: Finish — Rejected otherwise.
    return false;
}

void StatsOrchestrator::tick() {
    flusher_.fetch();
    flusher_.flush();
    if (state_ == OrchestratorState::Stopping) {
        finish();
    }
}

// Keeps the holder ("accepting" <=> Running) and the flusher in sync with the
// orchestrator state.
void StatsOrchestrator::setState(OrchestratorState state) {
    state_ = state;
    holder_.setAccepting(state == OrchestratorState::Running);
    flusher_.setOrchestratorState(state);
}

}  // namespace stats
