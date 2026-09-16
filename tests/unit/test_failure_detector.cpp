#include "master/failure_detector.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

StorageNodeInfo make_node(StorageNodeId id) {
    return StorageNodeInfo{id,   "127.0.0.1",
                            static_cast<uint16_t>(9100 + id),
                            NodeState::HEALTHY,
                            1024, 1024,
                            std::chrono::steady_clock::now()};
}

// Test-only timeouts — real ones (SUSPECTED_TIMEOUT_SECONDS=6,
// DEAD_TIMEOUT_SECONDS=12) would make this suite unbearably slow.
constexpr auto kSuspectedTimeout = std::chrono::milliseconds(60);
constexpr auto kDeadTimeout = std::chrono::milliseconds(120);
constexpr auto kSweepInterval = std::chrono::milliseconds(10);

}  // namespace

TEST(FailureDetectorTest, HealthyNodeStaysHealthyWhileHeartbeatsContinue) {
    StorageRegistry registry;
    registry.register_node(make_node(1));
    FailureDetector detector(registry, [](StorageNodeId) {}, kSuspectedTimeout, kDeadTimeout,
                              kSweepInterval);
    detector.start();

    auto deadline = std::chrono::steady_clock::now() + kDeadTimeout * 3;
    while (std::chrono::steady_clock::now() < deadline) {
        ASSERT_EQ(detector.process_heartbeat(1), ErrorCode::OK);
        std::this_thread::sleep_for(kSuspectedTimeout / 3);
    }
    detector.stop();

    auto node = registry.get_node(1);
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->state, NodeState::HEALTHY);
}

TEST(FailureDetectorTest, MissedHeartbeatsTransitionHealthyToSuspectedToDead) {
    StorageRegistry registry;
    registry.register_node(make_node(1));
    FailureDetector detector(registry, [](StorageNodeId) {}, kSuspectedTimeout, kDeadTimeout,
                              kSweepInterval);
    detector.start();

    // No further heartbeats — just wait past SUSPECTED_TIMEOUT.
    std::this_thread::sleep_for(kSuspectedTimeout + kSweepInterval * 3);
    {
        auto node = registry.get_node(1);
        ASSERT_TRUE(node.has_value());
        EXPECT_EQ(node->state, NodeState::SUSPECTED);
    }

    // Wait past DEAD_TIMEOUT (measured from the same original heartbeat).
    std::this_thread::sleep_for(kDeadTimeout);
    detector.stop();

    auto node = registry.get_node(1);
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->state, NodeState::DEAD);
}

TEST(FailureDetectorTest, OnNodeDeadFiresExactlyOnceForOneFailure) {
    StorageRegistry registry;
    registry.register_node(make_node(1));

    std::mutex calls_mutex;
    std::vector<StorageNodeId> calls;
    FailureDetector detector(
        registry,
        [&](StorageNodeId id) {
            std::lock_guard<std::mutex> lock(calls_mutex);
            calls.push_back(id);
        },
        kSuspectedTimeout, kDeadTimeout, kSweepInterval);
    detector.start();

    std::this_thread::sleep_for(kSuspectedTimeout + kDeadTimeout + kDeadTimeout);
    detector.stop();

    std::lock_guard<std::mutex> lock(calls_mutex);
    EXPECT_EQ(calls, (std::vector<StorageNodeId>{1}));
}

TEST(FailureDetectorTest, ClearNotificationAllowsOnNodeDeadToFireAgainForSameNode) {
    // Bugfix regression: without clear_notification(), a node's SECOND
    // death would never re-fire on_node_dead_ once notified_dead_ already
    // contains its id from the first death — silently defeating recovery
    // for any node that dies more than once in a Master's lifetime (see
    // docs/design.md §16).
    StorageRegistry registry;
    registry.register_node(make_node(1));

    std::mutex calls_mutex;
    std::vector<StorageNodeId> calls;
    FailureDetector detector(
        registry,
        [&](StorageNodeId id) {
            std::lock_guard<std::mutex> lock(calls_mutex);
            calls.push_back(id);
        },
        kSuspectedTimeout, kDeadTimeout, kSweepInterval);
    detector.start();

    // First death.
    std::this_thread::sleep_for(kSuspectedTimeout + kDeadTimeout + kDeadTimeout);
    {
        std::lock_guard<std::mutex> lock(calls_mutex);
        ASSERT_EQ(calls, (std::vector<StorageNodeId>{1}));
    }

    // Reconciliation promotes it back to HEALTHY (as
    // Master::handle_node_inventory() does) and clears the notification.
    registry.set_state(1, NodeState::HEALTHY);
    ASSERT_EQ(detector.process_heartbeat(1), ErrorCode::OK);
    detector.clear_notification(1);

    // Second death — must fire on_node_dead_ again, not just once total.
    std::this_thread::sleep_for(kSuspectedTimeout + kDeadTimeout + kDeadTimeout);
    detector.stop();

    std::lock_guard<std::mutex> lock(calls_mutex);
    EXPECT_EQ(calls, (std::vector<StorageNodeId>{1, 1}));
}

TEST(FailureDetectorTest, ProcessHeartbeatOnUnknownNodeReturnsNodeNotFound) {
    StorageRegistry registry;
    FailureDetector detector(registry);
    EXPECT_EQ(detector.process_heartbeat(99), ErrorCode::NODE_NOT_FOUND);
}

TEST(FailureDetectorTest, StopReturnsPromptlyWithoutWaitingOutSweepInterval) {
    StorageRegistry registry;
    FailureDetector detector(registry, [](StorageNodeId) {}, kSuspectedTimeout, kDeadTimeout,
                              std::chrono::seconds(30));
    detector.start();

    auto start = std::chrono::steady_clock::now();
    detector.stop();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, std::chrono::seconds(1));
}
