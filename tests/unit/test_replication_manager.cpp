#include "master/replication_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <set>
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

}  // namespace

TEST(ReplicationManagerTest, ChooseReplicasReturnsThreeDistinctNodesFromThreeRegistered) {
    StorageRegistry registry;
    registry.register_node(make_node(1));
    registry.register_node(make_node(2));
    registry.register_node(make_node(3));
    ReplicationManager manager(registry);

    auto replicas = manager.choose_replicas(/*block_id=*/1);
    ASSERT_EQ(replicas.size(), 3u);
    std::set<StorageNodeId> distinct(replicas.begin(), replicas.end());
    EXPECT_EQ(distinct.size(), 3u);
    EXPECT_EQ(distinct, (std::set<StorageNodeId>{1, 2, 3}));
}

TEST(ReplicationManagerTest, ChooseReplicasReturnsEmptyWhenFewerThanThreeAlive) {
    StorageRegistry registry;
    ReplicationManager manager0(registry);
    EXPECT_TRUE(manager0.choose_replicas(1).empty());

    registry.register_node(make_node(1));
    ReplicationManager manager1(registry);
    EXPECT_TRUE(manager1.choose_replicas(1).empty());

    registry.register_node(make_node(2));
    ReplicationManager manager2(registry);
    EXPECT_TRUE(manager2.choose_replicas(1).empty());
}

TEST(ReplicationManagerTest, ChooseReplicasIgnoresDeadNodes) {
    StorageRegistry registry;
    registry.register_node(make_node(1));
    registry.register_node(make_node(2));
    registry.register_node(make_node(3));
    registry.mark_dead(3);
    ReplicationManager manager(registry);

    EXPECT_TRUE(manager.choose_replicas(1).empty());
}

TEST(ReplicationManagerTest, ChooseReplicasRoundRobinsAcrossSuccessiveCalls) {
    StorageRegistry registry;
    for (StorageNodeId id = 1; id <= 5; ++id) {
        registry.register_node(make_node(id));
    }
    ReplicationManager manager(registry);

    auto first = manager.choose_replicas(1);
    auto second = manager.choose_replicas(2);

    ASSERT_EQ(first.size(), 3u);
    ASSERT_EQ(second.size(), 3u);
    EXPECT_EQ(first, (std::vector<StorageNodeId>{1, 2, 3}));
    EXPECT_EQ(second, (std::vector<StorageNodeId>{4, 5, 1}));
}

TEST(ReplicationManagerTest, ChooseReplicasRespectsConfiguredReplicationFactorOfOne) {
    // Faza 17.1: replication_factor is now a runtime Master config knob
    // (see docs/design.md §21) rather than always DEFAULT_REPLICATION_FACTOR.
    StorageRegistry registry;
    registry.register_node(make_node(1));
    registry.register_node(make_node(2));
    registry.register_node(make_node(3));
    ReplicationManager manager(registry, /*replication_factor=*/1);

    auto replicas = manager.choose_replicas(1);
    ASSERT_EQ(replicas.size(), 1u);
}

TEST(ReplicationManagerTest, ChooseReplicasRespectsConfiguredReplicationFactorOfTwo) {
    StorageRegistry registry;
    registry.register_node(make_node(1));
    registry.register_node(make_node(2));
    registry.register_node(make_node(3));
    ReplicationManager manager(registry, /*replication_factor=*/2);

    auto replicas = manager.choose_replicas(1);
    ASSERT_EQ(replicas.size(), 2u);
    std::set<StorageNodeId> distinct(replicas.begin(), replicas.end());
    EXPECT_EQ(distinct.size(), 2u);
}

TEST(ReplicationManagerTest, ChooseReplicasReturnsEmptyWhenFewerThanConfiguredFactorAlive) {
    StorageRegistry registry;
    registry.register_node(make_node(1));
    ReplicationManager manager(registry, /*replication_factor=*/2);

    EXPECT_TRUE(manager.choose_replicas(1).empty());
}

TEST(ReplicationManagerTest, ConcurrentChooseReplicasAlwaysReturnsThreeValidDistinctNodes) {
    // Exercises the cursor_ mutex under real concurrency (run under
    // ASan/UBSan as part of the usual verification pass) — every
    // concurrent call must still see exactly 3 distinct, actually-alive
    // node ids, regardless of interleaving.
    StorageRegistry registry;
    std::set<StorageNodeId> valid_ids;
    for (StorageNodeId id = 1; id <= 5; ++id) {
        registry.register_node(make_node(id));
        valid_ids.insert(id);
    }
    ReplicationManager manager(registry);

    constexpr int kCalls = 16;
    std::vector<std::thread> threads;
    std::vector<std::vector<StorageNodeId>> results(kCalls);
    for (int i = 0; i < kCalls; ++i) {
        threads.emplace_back([&manager, &results, i]() { results[i] = manager.choose_replicas(1); });
    }
    for (auto& t : threads) t.join();

    for (const auto& replicas : results) {
        ASSERT_EQ(replicas.size(), 3u);
        std::set<StorageNodeId> distinct(replicas.begin(), replicas.end());
        EXPECT_EQ(distinct.size(), 3u);
        for (StorageNodeId id : replicas) {
            EXPECT_TRUE(valid_ids.count(id) > 0);
        }
    }
}
