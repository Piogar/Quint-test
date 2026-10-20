#pragma once

// Builds one complete framework instance for tests: holder, fake repository,
// flusher, orchestrator and collectors created on demand.

#include <map>
#include <memory>

#include "InMemoryStatsRepository.h"
#include "StatsCollector.h"
#include "StatsFlusher.h"
#include "StatsHolder.h"
#include "StatsOrchestrator.h"
#include "StatsTypes.h"

namespace stats {

// Model values of spec/stats.qnt, module StatsFrameworkVerification
// (design doc: "Verification Only"). The traces are generated with them.
inline StatsConfig modelConfig() {
    StatsConfig config;
    config.holderCapacity = 2;
    config.flusherCapacity = 3;
    config.maxDurationUs = 10;
    return config;
}

struct TestSystem {
    explicit TestSystem(const StatsConfig& config = modelConfig())
        : holder(config.holderCapacity), flusher(holder, repository, config), orchestrator(holder, flusher) {}

    StatsCollector& collector(const CollectorId& id) {
        auto it = collectors.find(id);
        if (it == collectors.end()) {
            it = collectors.emplace(id, std::make_unique<StatsCollector>(id, holder)).first;
        }
        return *it->second;
    }

    StatsHolder holder;
    InMemoryStatsRepository repository;
    StatsFlusher flusher;
    StatsOrchestrator orchestrator;
    std::map<CollectorId, std::unique_ptr<StatsCollector>> collectors;
};

}  // namespace stats
