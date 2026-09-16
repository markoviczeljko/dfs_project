#include "master/storage_registry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace {

StorageNodeInfo make_node(StorageNodeId id, uint16_t port, NodeState state = NodeState::HEALTHY) {
    return StorageNodeInfo{id, "127.0.0.1", port, state, 1024, 1024, std::chrono::steady_clock::now()};
}

}  // namespace

TEST(StorageRegistryTest, GetAliveNodesEmptyInitially) {
    StorageRegistry registry;
    EXPECT_TRUE(registry.get_alive_nodes().empty());
}

TEST(StorageRegistryTest, RegisterNodeThenGetAliveNodesReturnsIt) {
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));

    auto alive = registry.get_alive_nodes();
    ASSERT_EQ(alive.size(), 1u);
    EXPECT_EQ(alive[0].id, 1u);
    EXPECT_EQ(alive[0].port, 9101);
}

TEST(StorageRegistryTest, RegisterNodeTwiceWithSameIdOverwritesEntry) {
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));
    registry.register_node(make_node(1, 9999));

    auto alive = registry.get_alive_nodes();
    ASSERT_EQ(alive.size(), 1u);
    EXPECT_EQ(alive[0].port, 9999);
}

TEST(StorageRegistryTest, GetNodeReturnsNulloptForUnknownId) {
    StorageRegistry registry;
    EXPECT_FALSE(registry.get_node(42).has_value());
}

TEST(StorageRegistryTest, GetNodeReturnsRegisteredNode) {
    StorageRegistry registry;
    registry.register_node(make_node(7, 9107));

    auto node = registry.get_node(7);
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->port, 9107);
}

TEST(StorageRegistryTest, MarkDeadExcludesNodeFromGetAliveNodes) {
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));
    registry.register_node(make_node(2, 9102));

    registry.mark_dead(1);

    auto alive = registry.get_alive_nodes();
    ASSERT_EQ(alive.size(), 1u);
    EXPECT_EQ(alive[0].id, 2u);
}

TEST(StorageRegistryTest, MarkAliveRestoresNodeToGetAliveNodes) {
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));
    registry.mark_dead(1);
    registry.mark_alive(1);

    auto alive = registry.get_alive_nodes();
    ASSERT_EQ(alive.size(), 1u);
    EXPECT_EQ(alive[0].id, 1u);
}

// Faza 12.1: NodeState / record_heartbeat (see docs/design.md §16).

TEST(StorageRegistryTest, RecordHeartbeatOnUnknownNodeReturnsNodeNotFound) {
    StorageRegistry registry;
    EXPECT_EQ(registry.record_heartbeat(42), ErrorCode::NODE_NOT_FOUND);
}

TEST(StorageRegistryTest, RecordHeartbeatOnKnownNodeSucceeds) {
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));
    EXPECT_EQ(registry.record_heartbeat(1), ErrorCode::OK);
}

TEST(StorageRegistryTest, RecordHeartbeatPromotesSuspectedBackToHealthy) {
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));
    registry.set_state(1, NodeState::SUSPECTED);

    ASSERT_EQ(registry.record_heartbeat(1), ErrorCode::OK);

    auto node = registry.get_node(1);
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->state, NodeState::HEALTHY);
}

TEST(StorageRegistryTest, RecordHeartbeatDoesNotPromoteDeadNode) {
    // Only inventory reconciliation (Faza 12.3) is allowed to bring a DEAD
    // node back — a bare heartbeat alone must not.
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));
    registry.set_state(1, NodeState::DEAD);

    ASSERT_EQ(registry.record_heartbeat(1), ErrorCode::OK);

    auto node = registry.get_node(1);
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->state, NodeState::DEAD);
}

TEST(StorageRegistryTest, GetNodesInStateFiltersCorrectly) {
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));
    registry.register_node(make_node(2, 9102));
    registry.register_node(make_node(3, 9103));
    registry.set_state(2, NodeState::SUSPECTED);
    registry.set_state(3, NodeState::DEAD);

    EXPECT_EQ(registry.get_nodes_in_state(NodeState::HEALTHY).size(), 1u);
    EXPECT_EQ(registry.get_nodes_in_state(NodeState::SUSPECTED).size(), 1u);
    EXPECT_EQ(registry.get_nodes_in_state(NodeState::DEAD).size(), 1u);
    EXPECT_EQ(registry.get_all_nodes().size(), 3u);
}

TEST(StorageRegistryTest, GetAliveNodesOnlyReturnsHealthyNotSuspectedOrDead) {
    StorageRegistry registry;
    registry.register_node(make_node(1, 9101));
    registry.register_node(make_node(2, 9102));
    registry.register_node(make_node(3, 9103));
    registry.set_state(2, NodeState::SUSPECTED);
    registry.set_state(3, NodeState::DEAD);

    auto alive = registry.get_alive_nodes();
    ASSERT_EQ(alive.size(), 1u);
    EXPECT_EQ(alive[0].id, 1u);
}

TEST(StorageRegistryTest, ConcurrentRegisterNodeWithDistinctIdsAllPersist) {
    // Exercises the shared_mutex under real concurrency (run under
    // ASan/UBSan as part of the usual verification pass) — every one of N
    // concurrently registered distinct ids must show up, none lost or
    // corrupted.
    StorageRegistry registry;
    constexpr int kNodes = 16;
    std::vector<std::thread> threads;
    for (int i = 0; i < kNodes; ++i) {
        threads.emplace_back(
            [&registry, i]() { registry.register_node(make_node(static_cast<StorageNodeId>(i + 1), static_cast<uint16_t>(9100 + i))); });
    }
    for (auto& t : threads) t.join();

    auto alive = registry.get_alive_nodes();
    EXPECT_EQ(alive.size(), static_cast<size_t>(kNodes));
    for (int i = 0; i < kNodes; ++i) {
        EXPECT_TRUE(registry.get_node(static_cast<StorageNodeId>(i + 1)).has_value());
    }
}
