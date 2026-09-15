#include "master/storage_registry.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace {

StorageNodeInfo make_node(StorageNodeId id, uint16_t port, bool alive = true) {
    return StorageNodeInfo{id, "127.0.0.1", port, alive, 1024, 1024};
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
