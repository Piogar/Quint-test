// MBT harness: replays the ITF traces generated from spec/stats.qnt
// (scripts/gen_traces.sh) against the C++ implementation.
//
// For every trace: build a fresh framework, then for every step
//   - call the operation named in `mbt::actionTaken` with the arguments from
//     `mbt::nondetPicks`,
//   - BG-01: compare the value returned by the C++ method with `lastResult`,
//   - BG-02: compare the C++ state with the spec state.
// Not compared (verification only): `ghost*` variables, the
// StoreStatsRecordResult label (the C++ method returns void), `nextSeq`
// (checked through the `seq` values of the samples).
// Notes: conclusions.md, "5.1 Notes for the harness prompt".

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "StatsTestAccess.h"
#include "TestSystem.h"

namespace stats {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
using Records = std::vector<StatRecord>;

// ------------------------------------------------------------ ITF decoding

// Variable names are fully qualified, e.g.
// "StatsFrameworkVerification::StatsFramework::collected" -> "collected".
std::string shortName(const std::string& key) {
    const auto pos = key.rfind("::");
    return pos == std::string::npos ? key : key.substr(pos + 2);
}

// Integers are encoded as {"#bigint": "..."} (or plain JSON numbers).
int64_t asInt(const json& v) {
    if (v.is_object() && v.contains("#bigint")) {
        return std::stoll(v.at("#bigint").get<std::string>());
    }
    return v.get<int64_t>();
}

StatRecord asRecord(const json& v) {
    return StatRecord{v.at("collector").get<std::string>(), v.at("procedure").get<std::string>(),
                      asInt(v.at("seq")), asInt(v.at("durationUs"))};
}

Records asRecords(const json& v) {
    Records records;
    for (const json& item : v) {
        records.push_back(asRecord(item));
    }
    return records;
}

// Maps are encoded as {"#map": [[key, value], ...]}.
std::map<std::string, int64_t> asCounterMap(const json& v) {
    std::map<std::string, int64_t> result;
    for (const json& entry : v.at("#map")) {
        result[entry.at(0).get<std::string>()] = asInt(entry.at(1));
    }
    return result;
}

std::string tagOf(const json& v) { return v.at("tag").get<std::string>(); }

// Option values in `mbt::nondetPicks`: {"tag": "Some", "value": x} or {"tag": "None", ...}.
const json& pick(const json& picks, const std::string& name) {
    const json& p = picks.at(name);
    if (tagOf(p) != "Some") {
        throw std::runtime_error("nondet pick '" + name + "' is None");
    }
    return p.at("value");
}

std::string toString(const StatRecord& r) {
    std::ostringstream os;
    os << "{" << r.collector << "," << r.procedure << ",seq=" << r.seq << ",d=" << r.durationUs << "}";
    return os.str();
}

std::string toString(const Records& records) {
    std::string s = "[";
    for (std::size_t i = 0; i < records.size(); ++i) {
        s += (i ? ", " : "") + toString(records[i]);
    }
    return s + "]";
}

std::string toString(OrchestratorState s) {
    switch (s) {
        case OrchestratorState::Stopped: return "Stopped";
        case OrchestratorState::Running: return "Running";
        case OrchestratorState::Stopping: return "Stopping";
    }
    return "?";
}

std::string toString(const std::optional<std::size_t>& v) { return v ? std::to_string(*v) : "nullopt"; }

// ------------------------------------------------------------ comparisons

// One spec state with short variable names (mbt:: and ghost* kept but unused).
std::map<std::string, json> specVars(const json& itfState) {
    std::map<std::string, json> vars;
    for (const auto& [key, value] : itfState.items()) {
        if (key.rfind("#", 0) == 0) continue;
        vars[key.rfind("mbt::", 0) == 0 ? key : shortName(key)] = value;
    }
    return vars;
}

// BG-02 StateConformance. Returns a description of the first difference.
std::optional<std::string> compareState(const std::map<std::string, json>& spec, TestSystem& sys) {
    const std::string expState = tagOf(spec.at("state"));
    if (expState != toString(sys.orchestrator.state())) {
        return "state: expected " + expState + ", actual " + toString(sys.orchestrator.state());
    }
    const auto check = [](const char* name, const Records& expected, const Records& actual) -> std::optional<std::string> {
        if (expected != actual) {
            return std::string(name) + ": expected " + toString(expected) + ", actual " + toString(actual);
        }
        return std::nullopt;
    };
    if (auto d = check("collected", asRecords(spec.at("collected")), StatsTestAccess::collected(sys.holder))) return d;
    if (auto d = check("processing", asRecords(spec.at("processing")), StatsTestAccess::processing(sys.flusher))) return d;
    if (auto d = check("stored", asRecords(spec.at("stored")), sys.repository.stored())) return d;

    if (spec.at("repoAvailable").get<bool>() != sys.repository.available()) {
        return "repoAvailable differs";
    }
    struct Counter {
        const char* name;
        int64_t (*actual)(TestSystem&, const CollectorId&);
    };
    const Counter counters[] = {
        {"dropped", [](TestSystem& s, const CollectorId& c) { return s.holder.dropped(c); }},
        {"overflowed", [](TestSystem& s, const CollectorId& c) { return s.flusher.overflowed(c); }},
        {"discarded", [](TestSystem& s, const CollectorId& c) { return s.flusher.discarded(c); }},
        {"lost", [](TestSystem& s, const CollectorId& c) { return s.flusher.lost(c); }},
    };
    for (const Counter& counter : counters) {
        for (const auto& [collector, expected] : asCounterMap(spec.at(counter.name))) {
            const int64_t actual = counter.actual(sys, collector);
            if (actual != expected) {
                return std::string(counter.name) + "[" + collector + "]: expected " + std::to_string(expected) +
                       ", actual " + std::to_string(actual);
            }
        }
    }
    return std::nullopt;
}

// Executes one step on the C++ implementation and checks its result against
// `lastResult` (BG-01). Returns a description of a difference, if any.
std::optional<std::string> executeStep(const std::string& action, const json& picks, const json& lastResult,
                                       TestSystem& sys) {
    const auto expectBool = [&](const char* tag, bool actual) -> std::optional<std::string> {
        if (tagOf(lastResult) != tag) {
            return "lastResult: expected tag " + std::string(tag) + ", trace has " + tagOf(lastResult);
        }
        const bool expected = lastResult.at("value").get<bool>();
        if (expected != actual) {
            return std::string(tag) + ": expected " + (expected ? "true" : "false") + ", actual " +
                   (actual ? "true" : "false");
        }
        return std::nullopt;
    };

    if (action == "doStart") return expectBool("StartResult", sys.orchestrator.start());
    if (action == "doStop") return expectBool("StopResult", sys.orchestrator.stop());
    if (action == "doFinish") return expectBool("FinishResult", sys.orchestrator.finish());
    if (action == "doFlush") return expectBool("FlushResult", sys.flusher.flush());
    if (action == "doFetch") {
        const std::optional<std::size_t> actual = sys.flusher.fetch();
        const json& value = lastResult.at("value");
        const std::optional<std::size_t> expected =
            value.at("ok").get<bool>() ? std::optional<std::size_t>(static_cast<std::size_t>(asInt(value.at("n"))))
                                       : std::nullopt;
        if (expected != actual) {
            return "FetchResult: expected " + toString(expected) + ", actual " + toString(actual);
        }
        return std::nullopt;
    }
    if (action == "doStoreStatsRecord") {
        // void: the StoreStatsRecordResult label is verification only.
        sys.collector(pick(picks, "c").get<std::string>())
            .storeStatsRecord(pick(picks, "p").get<std::string>(), asInt(pick(picks, "d")));
        return std::nullopt;
    }
    if (action == "doSetRepositoryAvailable") {
        // Environment step: driven through the fake repository.
        sys.repository.setAvailable(pick(picks, "b").get<bool>());
        return std::nullopt;
    }
    return "unknown action '" + action + "'";
}

// ------------------------------------------------------------ trace files

std::vector<std::string> traceFiles() {
    std::vector<std::string> files;
    for (const char* sub : {"random", "witness"}) {
        const fs::path dir = fs::path(TRACES_DIR) / sub;
        if (!fs::exists(dir)) continue;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".json") {
                files.push_back(entry.path().string());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string testName(const ::testing::TestParamInfo<std::string>& info) {
    const fs::path p(info.param);
    std::string name = p.parent_path().filename().string() + "_" + p.stem().stem().string();
    std::replace_if(name.begin(), name.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return name;
}

// ------------------------------------------------------------ tests

TEST(MbtTraces, TracesArePresent) {
    EXPECT_GT(traceFiles().size(), 0u) << "no traces in " << TRACES_DIR << " (run scripts/gen_traces.sh)";
}

class MbtTraceTest : public ::testing::TestWithParam<std::string> {};

TEST_P(MbtTraceTest, ImplementationFollowsSpec) {
    std::ifstream in(GetParam());
    ASSERT_TRUE(in) << "cannot open " << GetParam();
    const json trace = json::parse(in);
    const json& states = trace.at("states");
    ASSERT_FALSE(states.empty());

    TestSystem sys;  // fresh framework per trace, model values of the spec

    for (std::size_t i = 0; i < states.size(); ++i) {
        const auto spec = specVars(states[i]);
        const std::string action = spec.at("mbt::actionTaken").get<std::string>();

        if (i == 0) {
            ASSERT_EQ(action, "init");
        } else if (auto diff = executeStep(action, spec.at("mbt::nondetPicks"), spec.at("lastResult"), sys)) {
            FAIL() << "step " << i << " (" << action << "), BG-01 result: " << *diff;
        }
        if (auto diff = compareState(spec, sys)) {
            FAIL() << "step " << i << " (" << action << "), BG-02 state: " << *diff;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Traces, MbtTraceTest, ::testing::ValuesIn(traceFiles()), testName);

}  // namespace
}  // namespace stats
