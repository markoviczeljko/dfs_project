#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <thread>
#include <vector>

#include "storage/block_manager.h"

class BlockManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_block_manager_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(BlockManagerTest, AllocateReturnsUniqueSequentialIds) {
    auto result = BlockManager::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto manager = std::move(result.value());

    auto first = manager->allocate_block();
    auto second = manager->allocate_block();
    auto third = manager->allocate_block();

    ASSERT_TRUE(first.is_ok());
    ASSERT_TRUE(second.is_ok());
    ASSERT_TRUE(third.is_ok());
    EXPECT_NE(first.value(), second.value());
    EXPECT_NE(second.value(), third.value());
    EXPECT_NE(first.value(), third.value());
}

TEST_F(BlockManagerTest, ConcurrentAllocateProducesNoDuplicatesOrLoss) {
    auto result = BlockManager::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto manager = std::move(result.value());

    constexpr int kThreadCount = 8;
    constexpr int kAllocationsPerThread = 50;

    std::vector<std::vector<BlockId>> per_thread_ids(kThreadCount);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreadCount; ++t) {
        threads.emplace_back([&manager, &per_thread_ids, t]() {
            for (int i = 0; i < kAllocationsPerThread; ++i) {
                auto allocated = manager->allocate_block();
                ASSERT_TRUE(allocated.is_ok());
                per_thread_ids[t].push_back(allocated.value());
            }
        });
    }
    for (auto& thread : threads) thread.join();

    std::set<BlockId> all_ids;
    size_t total = 0;
    for (const auto& ids : per_thread_ids) {
        for (BlockId id : ids) {
            all_ids.insert(id);
            ++total;
        }
    }

    EXPECT_EQ(total, static_cast<size_t>(kThreadCount * kAllocationsPerThread));
    EXPECT_EQ(all_ids.size(), total);
}

TEST_F(BlockManagerTest, WriteAndReadRoundTripThroughManager) {
    auto result = BlockManager::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto manager = std::move(result.value());

    auto allocated = manager->allocate_block();
    ASSERT_TRUE(allocated.is_ok());

    std::vector<uint8_t> data = {10, 20, 30, 40, 50};
    EXPECT_EQ(manager->write_block(allocated.value(), data), ErrorCode::OK);

    auto read_result = manager->read_block(allocated.value());
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(BlockManagerTest, WriteBlockWorksWithExternallyProvidedId) {
    auto result = BlockManager::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto manager = std::move(result.value());

    // Simulates a future Master-issued BlockId that never went through this
    // node's own allocate_block() — must still work (see docs/design.md).
    BlockId external_id = 999999;
    std::vector<uint8_t> data = {1, 2, 3};

    EXPECT_EQ(manager->write_block(external_id, data), ErrorCode::OK);

    auto read_result = manager->read_block(external_id);
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(BlockManagerTest, WriteBlockAtDelegatesToStorage) {
    // Thin passthrough — full write_block_at() coverage lives in
    // test_block_storage.cpp (see docs/design.md §14).
    auto result = BlockManager::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto manager = std::move(result.value());

    auto allocated = manager->allocate_block();
    ASSERT_TRUE(allocated.is_ok());
    BlockId id = allocated.value();

    ASSERT_EQ(manager->write_block(id, std::vector<uint8_t>{1, 2, 3}), ErrorCode::OK);
    EXPECT_EQ(manager->write_block_at(id, 1, std::vector<uint8_t>{9}), ErrorCode::OK);

    auto read_result = manager->read_block(id);
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), (std::vector<uint8_t>{1, 9, 3}));
}

TEST_F(BlockManagerTest, ReleaseBlockRemovesDataAndIsIdempotent) {
    auto result = BlockManager::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto manager = std::move(result.value());

    auto allocated = manager->allocate_block();
    ASSERT_TRUE(allocated.is_ok());
    BlockId id = allocated.value();

    std::vector<uint8_t> data = {7, 7, 7};
    ASSERT_EQ(manager->write_block(id, data), ErrorCode::OK);

    EXPECT_EQ(manager->release_block(id), ErrorCode::OK);

    auto read_result = manager->read_block(id);
    ASSERT_TRUE(read_result.is_error());
    EXPECT_EQ(read_result.error(), ErrorCode::BLOCK_NOT_FOUND);

    // Releasing again (already released) must still succeed.
    EXPECT_EQ(manager->release_block(id), ErrorCode::OK);
}
