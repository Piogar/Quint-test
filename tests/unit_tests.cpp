// Unit tests: success and every rejection branch of each operation, mirroring
// the scenario tests in spec/stats.qnt (module StatsFrameworkVerification).

#include <gtest/gtest.h>

#include <optional>

#include "StatsTestAccess.h"
#include "TestSystem.h"

namespace stats {
namespace {

using Records = std::vector<StatRecord>;

StatRecord rec(const CollectorId& c, const ProcedureId& p, int64_t seq, int64_t d) { return StatRecord{c, p, seq, d}; }

class StatsFrameworkTest : public ::testing::Test {
protected:
    Records collected() const { return StatsTestAccess::collected(sys.holder); }
    Records processing() const { return StatsTestAccess::processing(sys.flusher); }
    const Records& stored() const { return sys.repository.stored(); }
    void store(const CollectorId& c, const ProcedureId& p, int64_t d) { sys.collector(c).storeStatsRecord(p, d); }

    TestSystem sys;
};

// ---------------------------------------------------------------- Start / Stop

TEST_F(StatsFrameworkTest, StartSucceedsWhenStoppedAndIsRejectedOtherwise) {
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Stopped);
    EXPECT_TRUE(sys.orchestrator.start());
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Running);
    EXPECT_FALSE(sys.orchestrator.start());  // Rejected: state != Stopped
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Running);
}

TEST_F(StatsFrameworkTest, StopSucceedsWhenRunningAndIsRejectedOtherwise) {
    EXPECT_FALSE(sys.orchestrator.stop());  // Rejected: Stopped
    sys.orchestrator.start();
    EXPECT_TRUE(sys.orchestrator.stop());
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Stopping);
    EXPECT_FALSE(sys.orchestrator.stop());  // Rejected: Stopping
}

// ----------------------------------------------------------- StoreStatsRecord

TEST_F(StatsFrameworkTest, StoreIsIgnoredWhenNotRunning) {
    store("c1", "p1", 5);  // Stopped
    EXPECT_TRUE(collected().empty());
    EXPECT_EQ(sys.holder.dropped("c1"), 0);

    sys.orchestrator.start();
    sys.orchestrator.stop();
    store("c1", "p1", 5);  // Stopping
    EXPECT_TRUE(collected().empty());

    // The ignored calls did not consume sequence numbers.
    sys.orchestrator.finish();
    sys.orchestrator.start();
    store("c1", "p1", 5);
    EXPECT_EQ(collected(), (Records{rec("c1", "p1", 0, 5)}));
}

TEST_F(StatsFrameworkTest, StoreAppendsWithPerCollectorSequenceNumbers) {
    sys.orchestrator.start();
    store("c1", "p1", 5);
    store("c2", "p2", 7);
    EXPECT_EQ(collected(), (Records{rec("c1", "p1", 0, 5), rec("c2", "p2", 0, 7)}));
}

// W-01 HolderFull (holderFullTest)
TEST_F(StatsFrameworkTest, StoreIsDroppedWhenHolderFull) {
    sys.orchestrator.start();
    store("c1", "p1", 0);
    store("c2", "p2", 0);
    store("c2", "p2", 0);  // holder capacity 2: dropped
    EXPECT_EQ(collected().size(), 2u);
    EXPECT_EQ(sys.holder.dropped("c2"), 1);
    EXPECT_EQ(sys.holder.dropped("c1"), 0);
    // The dropped sample did not consume a sequence number.
    sys.flusher.fetch();
    store("c2", "p2", 0);
    EXPECT_EQ(collected(), (Records{rec("c2", "p2", 1, 0)}));
}

// ----------------------------------------------------------------------- Fetch

TEST_F(StatsFrameworkTest, FetchIsRejectedWhenStoppedOrHolderEmpty) {
    EXPECT_EQ(sys.flusher.fetch(), std::nullopt);  // Stopped
    sys.orchestrator.start();
    EXPECT_EQ(sys.flusher.fetch(), std::nullopt);  // collected empty
}

TEST_F(StatsFrameworkTest, FetchMovesAllCollectedSamplesBehindProcessing) {
    sys.orchestrator.start();
    sys.repository.setAvailable(false);
    store("c1", "p1", 5);
    EXPECT_EQ(sys.flusher.fetch(), std::optional<std::size_t>(1));
    sys.flusher.flush();  // fails, sample kept
    store("c1", "p2", 0);
    EXPECT_EQ(sys.flusher.fetch(), std::optional<std::size_t>(1));
    EXPECT_TRUE(collected().empty());
    EXPECT_EQ(processing(), (Records{rec("c1", "p1", 0, 5), rec("c1", "p2", 1, 0)}));
}

// W-07 FlusherFull (flusherFullTest)
TEST_F(StatsFrameworkTest, FetchOverflowsWhenProcessingIsFull) {
    sys.orchestrator.start();
    sys.repository.setAvailable(false);
    store("c1", "p1", 5);
    store("c1", "p1", 5);
    sys.flusher.fetch();
    sys.flusher.flush();
    store("c2", "p1", 5);
    store("c2", "p1", 5);
    EXPECT_EQ(sys.flusher.fetch(), std::optional<std::size_t>(1));  // flusher capacity 3
    EXPECT_EQ(processing().size(), 3u);
    EXPECT_TRUE(collected().empty());
    EXPECT_EQ(sys.flusher.overflowed("c2"), 1);
}

TEST_F(StatsFrameworkTest, FetchMovesNothingWhenProcessingAlreadyFull) {
    sys.orchestrator.start();
    sys.repository.setAvailable(false);
    store("c1", "p1", 5);
    store("c1", "p1", 5);
    sys.flusher.fetch();
    store("c1", "p1", 5);
    sys.flusher.fetch();  // processing now 3 (full)
    store("c2", "p1", 5);
    EXPECT_EQ(sys.flusher.fetch(), std::optional<std::size_t>(0));
    EXPECT_EQ(sys.flusher.overflowed("c2"), 1);
    EXPECT_TRUE(collected().empty());
}

// ----------------------------------------------------------------------- Flush

TEST_F(StatsFrameworkTest, FlushIsRejectedWhenNothingToFlush) {
    sys.orchestrator.start();
    EXPECT_FALSE(sys.flusher.flush());
}

// W-02 FlushRetried (flushRetriedTest)
TEST_F(StatsFrameworkTest, FlushKeepsSamplesWhileRunningAndRetrySucceeds) {
    sys.orchestrator.start();
    sys.repository.setAvailable(false);
    store("c1", "p1", 5);
    sys.flusher.fetch();
    EXPECT_FALSE(sys.flusher.flush());
    EXPECT_EQ(processing().size(), 1u);
    EXPECT_TRUE(stored().empty());
    store("c1", "p2", 0);
    sys.flusher.fetch();
    sys.repository.setAvailable(true);
    EXPECT_TRUE(sys.flusher.flush());
    EXPECT_EQ(stored(), (Records{rec("c1", "p1", 0, 5), rec("c1", "p2", 1, 0)}));
    EXPECT_TRUE(processing().empty());
}

// W-03 InvalidDiscarded (invalidDiscardedTest)
TEST_F(StatsFrameworkTest, FlushDiscardsInvalidSamples) {
    sys.orchestrator.start();
    store("c1", "p1", -1);
    store("c2", "p1", 10);
    sys.flusher.fetch();
    EXPECT_TRUE(sys.flusher.flush());
    store("c1", "p1", 11);
    sys.flusher.fetch();
    EXPECT_TRUE(sys.flusher.flush());
    EXPECT_EQ(stored(), (Records{rec("c2", "p1", 0, 10)}));
    EXPECT_EQ(sys.flusher.discarded("c1"), 2);
}

// W-04 StopWithRepositoryDown (stopWithRepositoryDownTest)
TEST_F(StatsFrameworkTest, FlushLosesSamplesWhileStoppingWithRepositoryDown) {
    sys.orchestrator.start();
    store("c1", "p1", 5);
    sys.repository.setAvailable(false);
    sys.orchestrator.stop();
    store("c2", "p1", 5);  // ignored
    EXPECT_EQ(collected().size(), 1u);
    sys.flusher.fetch();
    EXPECT_FALSE(sys.flusher.flush());
    EXPECT_TRUE(processing().empty());
    EXPECT_EQ(sys.flusher.lost("c1"), 1);
    EXPECT_TRUE(sys.orchestrator.finish());
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Stopped);
    EXPECT_TRUE(stored().empty());
}

// ---------------------------------------------------------------------- Finish

// W-06 StopDrainsToRepository (stopDrainsToRepositoryTest)
TEST_F(StatsFrameworkTest, FinishOnlyAfterDrainingAndStopStoresPendingSamples) {
    sys.orchestrator.start();
    store("c1", "p1", 5);
    store("c2", "p2", 5);
    sys.orchestrator.stop();
    EXPECT_FALSE(sys.orchestrator.finish());  // collected not empty
    sys.flusher.fetch();
    EXPECT_FALSE(sys.orchestrator.finish());  // processing not empty
    EXPECT_TRUE(sys.flusher.flush());
    EXPECT_TRUE(sys.orchestrator.finish());
    EXPECT_EQ(stored().size(), 2u);
    EXPECT_EQ(sys.flusher.lost("c1") + sys.flusher.lost("c2"), 0);
}

TEST_F(StatsFrameworkTest, FinishIsRejectedWhenNotStopping) {
    EXPECT_FALSE(sys.orchestrator.finish());  // Stopped
    sys.orchestrator.start();
    EXPECT_FALSE(sys.orchestrator.finish());  // Running
}

// W-05 FullCycle (fullCycleTest)
TEST_F(StatsFrameworkTest, FullCycle) {
    EXPECT_TRUE(sys.orchestrator.start());
    store("c1", "p1", 5);
    EXPECT_EQ(sys.flusher.fetch(), std::optional<std::size_t>(1));
    EXPECT_TRUE(sys.flusher.flush());
    EXPECT_TRUE(sys.orchestrator.stop());
    EXPECT_TRUE(sys.orchestrator.finish());
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Stopped);
    EXPECT_EQ(stored(), (Records{rec("c1", "p1", 0, 5)}));
}

// ------------------------------------------------------- Flusher conversion rules

// isValidBoundariesTest
TEST_F(StatsFrameworkTest, ConvertKeepsOnlyValidSamplesInOrder) {
    const Records batch{rec("c1", "p1", 0, -1), rec("c1", "p1", 1, 0), rec("c1", "p1", 2, 10),
                        rec("c1", "p1", 3, 11)};
    EXPECT_EQ(sys.flusher.convert(batch), (Records{rec("c1", "p1", 1, 0), rec("c1", "p1", 2, 10)}));
}

// ------------------------------------------------------------------------ tick

TEST_F(StatsFrameworkTest, TickFetchesFlushesAndFinishesWhileStopping) {
    sys.orchestrator.start();
    store("c1", "p1", 5);
    sys.orchestrator.tick();  // fetch + flush
    EXPECT_EQ(stored().size(), 1u);
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Running);

    store("c1", "p1", 6);
    sys.orchestrator.stop();
    sys.orchestrator.tick();  // fetch + flush + finish
    EXPECT_EQ(stored().size(), 2u);
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Stopped);
}

// TP-01 StopTerminates: one tick after Stop reaches Stopped even with the
// repository down.
TEST_F(StatsFrameworkTest, TickStopsEvenWithRepositoryDown) {
    sys.orchestrator.start();
    store("c1", "p1", 5);
    sys.repository.setAvailable(false);
    sys.orchestrator.stop();
    sys.orchestrator.tick();
    EXPECT_EQ(sys.orchestrator.state(), OrchestratorState::Stopped);
    EXPECT_EQ(sys.flusher.lost("c1"), 1);
}

}  // namespace
}  // namespace stats
