#include "statistics/statistics.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "statistics/histogram.h"

TEST(HistogramTest, EmptyHistogramReturnsZeroPercentile) {
    Histogram h;
    EXPECT_EQ(h.percentile(0.50), 0u);
    EXPECT_EQ(h.percentile(0.99), 0u);
    EXPECT_EQ(h.count(), 0u);
}

TEST(HistogramTest, SingleSampleSatisfiesEveryPercentile) {
    Histogram h;
    h.record(std::chrono::microseconds(100));
    EXPECT_EQ(h.count(), 1u);
    EXPECT_GT(h.percentile(0.50), 0u);
    EXPECT_EQ(h.percentile(0.50), h.percentile(0.99));
}

TEST(HistogramTest, P99IsGreaterThanOrEqualToP50UnderSkewedDistribution) {
    Histogram h;
    // 95 fast samples (~10us) + 5 slow outliers (~100ms) — a nearest-rank
    // P99 over 100 samples lands at rank ceil(0.99*100)=99, which must be
    // one of the 5 slow ones (ranks 96-100) for this to actually exercise
    // tail detection; a 99/1 split would put P99 right at the boundary of
    // the fast cluster instead (rank 99 of 100 is still "fast" then).
    for (int i = 0; i < 95; ++i) {
        h.record(std::chrono::microseconds(10));
    }
    for (int i = 0; i < 5; ++i) {
        h.record(std::chrono::microseconds(100000));
    }

    uint64_t p50 = h.percentile(0.50);
    uint64_t p99 = h.percentile(0.99);
    EXPECT_LT(p50, 100u);
    EXPECT_GE(p99, 65536u);  // must land in the outlier's bucket (>= 2^16)
    EXPECT_GT(p99, p50);
}

TEST(HistogramTest, PercentilesAreMonotonicallyNonDecreasing) {
    Histogram h;
    for (int i = 1; i <= 1000; ++i) {
        h.record(std::chrono::microseconds(i));
    }
    EXPECT_LE(h.percentile(0.50), h.percentile(0.90));
    EXPECT_LE(h.percentile(0.90), h.percentile(0.95));
    EXPECT_LE(h.percentile(0.95), h.percentile(0.99));
}

TEST(StatisticsManagerTest, RecordReadUpdatesTotalsSuccessAndBytes) {
    StatisticsManager stats;
    stats.record_read(1024, std::chrono::microseconds(50), true);
    stats.record_read(2048, std::chrono::microseconds(50), false);

    StatisticsSnapshot snap = stats.snapshot();
    const OperationStats& read = snap.operations[static_cast<size_t>(OperationType::READ)];
    EXPECT_EQ(read.total, 2u);
    EXPECT_EQ(read.successful, 1u);
    EXPECT_EQ(read.failed, 1u);
    EXPECT_EQ(read.bytes, 1024u + 2048u);
}

TEST(StatisticsManagerTest, DifferentOperationTypesAreTrackedIndependently) {
    StatisticsManager stats;
    stats.record_read(10, std::chrono::microseconds(1), true);
    stats.record_write(20, std::chrono::microseconds(1), true);
    stats.record_append(30, std::chrono::microseconds(1), true);

    StatisticsSnapshot snap = stats.snapshot();
    EXPECT_EQ(snap.operations[static_cast<size_t>(OperationType::READ)].total, 1u);
    EXPECT_EQ(snap.operations[static_cast<size_t>(OperationType::WRITE)].total, 1u);
    EXPECT_EQ(snap.operations[static_cast<size_t>(OperationType::APPEND)].total, 1u);
    EXPECT_EQ(snap.operations[static_cast<size_t>(OperationType::CREATE)].total, 0u);
}

TEST(StatisticsManagerTest, RecordErrorTracksByErrorCode) {
    StatisticsManager stats;
    stats.record_error(ErrorCode::FILE_NOT_FOUND);
    stats.record_error(ErrorCode::FILE_NOT_FOUND);
    stats.record_error(ErrorCode::TIMEOUT);

    StatisticsSnapshot snap = stats.snapshot();
    EXPECT_EQ(snap.errors_by_code[static_cast<size_t>(ErrorCode::FILE_NOT_FOUND)], 2u);
    EXPECT_EQ(snap.errors_by_code[static_cast<size_t>(ErrorCode::TIMEOUT)], 1u);
    EXPECT_EQ(snap.errors_by_code[static_cast<size_t>(ErrorCode::OK)], 0u);
}

TEST(StatisticsManagerTest, RecordReplicationAccumulatesOperationsAndBytes) {
    StatisticsManager stats;
    stats.record_replication(1000);
    stats.record_replication(2000);

    StatisticsSnapshot snap = stats.snapshot();
    EXPECT_EQ(snap.replication_operations, 2u);
    EXPECT_EQ(snap.bytes_replicated, 3000u);
}

TEST(StatisticsManagerTest, RecordRecoveryOnlyCountsBytesOnSuccess) {
    StatisticsManager stats;
    stats.record_recovery(500, true);
    stats.record_recovery(999, false);  // failed attempt — bytes must not count

    StatisticsSnapshot snap = stats.snapshot();
    EXPECT_EQ(snap.recovery_operations, 2u);
    EXPECT_EQ(snap.successful_recoveries, 1u);
    EXPECT_EQ(snap.bytes_recovered, 500u);
}

TEST(StatisticsManagerTest, RecordNodeDeadIncrementsDetectedFailures) {
    StatisticsManager stats;
    stats.record_node_dead();
    stats.record_node_dead();
    EXPECT_EQ(stats.snapshot().detected_failures, 2u);
}

TEST(StatisticsManagerTest, HeartbeatCountersTrackSentReceivedAndFailures) {
    StatisticsManager stats;
    stats.record_heartbeat_sent(true);
    stats.record_heartbeat_sent(false);
    stats.record_heartbeat_received();

    StatisticsSnapshot snap = stats.snapshot();
    EXPECT_EQ(snap.heartbeats_sent, 2u);
    EXPECT_EQ(snap.heartbeat_send_failures, 1u);
    EXPECT_EQ(snap.heartbeats_received, 1u);
}

TEST(StatisticsManagerTest, StorageCapacityIsAGaugeNotAnAccumulator) {
    StatisticsManager stats;
    stats.record_storage_capacity(1000, 100, 1);
    stats.record_storage_capacity(1000, 250, 3);

    StatisticsSnapshot snap = stats.snapshot();
    EXPECT_EQ(snap.storage_capacity_bytes, 1000u);
    EXPECT_EQ(snap.storage_used_bytes, 250u);
    EXPECT_EQ(snap.storage_block_count, 3u);
}

TEST(StatisticsManagerTest, SlowOperationsProduceHigherP99ThanP50) {
    StatisticsManager stats;
    // See HistogramTest.P99IsGreaterThanOrEqualToP50UnderSkewedDistribution
    // for why a 95/5 split (not 99/1) is needed to land P99's nearest-rank
    // inside the slow cluster.
    for (int i = 0; i < 95; ++i) {
        stats.record_read(1, std::chrono::microseconds(10), true);
    }
    for (int i = 0; i < 5; ++i) {
        stats.record_read(1, std::chrono::microseconds(100000), true);
    }

    StatisticsSnapshot snap = stats.snapshot();
    const OperationStats& read = snap.operations[static_cast<size_t>(OperationType::READ)];
    EXPECT_GT(read.p99_us, read.p50_us);
}

TEST(StatisticsManagerTest, FormatSnapshotContainsExpectedSections) {
    StatisticsManager stats;
    stats.record_read(10, std::chrono::microseconds(5), true);
    stats.record_error(ErrorCode::FILE_NOT_FOUND);
    stats.record_replication(100);
    stats.record_recovery(50, true);
    stats.record_node_dead();
    stats.record_heartbeat_sent(true);
    stats.record_storage_capacity(1000, 200, 2);

    std::string text = stats.format_snapshot();
    EXPECT_NE(text.find("DFS Statistics"), std::string::npos);
    EXPECT_NE(text.find("READ:"), std::string::npos);
    EXPECT_NE(text.find("FILE_NOT_FOUND"), std::string::npos);
    EXPECT_NE(text.find("Replication:"), std::string::npos);
    EXPECT_NE(text.find("Recovery:"), std::string::npos);
    EXPECT_NE(text.find("Failures: detected=1"), std::string::npos);
    EXPECT_NE(text.find("Heartbeats:"), std::string::npos);
    EXPECT_NE(text.find("Storage:"), std::string::npos);
}

TEST(StatisticsManagerTest, ConcurrentRecordCallsAreNotLost) {
    StatisticsManager stats;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&stats]() {
            for (int i = 0; i < kPerThread; ++i) {
                stats.record_write(1, std::chrono::microseconds(1), true);
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(stats.snapshot().operations[static_cast<size_t>(OperationType::WRITE)].total,
              static_cast<uint64_t>(kThreads * kPerThread));
}
