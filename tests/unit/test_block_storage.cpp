#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "common/config.h"
#include "storage/block_header.h"
#include "storage/block_storage.h"

class BlockStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_block_storage_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(BlockStorageTest, WriteThenReadRoundTrip) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    std::vector<uint8_t> data = {1, 2, 3, 4, 5, 250, 251, 252};
    EXPECT_EQ(storage->write_block(42, data), ErrorCode::OK);

    auto read_result = storage->read_block(42);
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(BlockStorageTest, CorruptedBlockDetected) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    std::vector<uint8_t> data(100, 0xAB);
    ASSERT_EQ(storage->write_block(7, data), ErrorCode::OK);

    // Corrupt one data byte directly on disk, bypassing the BlockStorage API.
    std::filesystem::path block_file = dir_ / "blocks" / "7.block";
    std::fstream file(block_file, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file.seekp(static_cast<std::streamoff>(BLOCK_HEADER_SIZE));
    char corrupted = static_cast<char>(0xFF);
    file.write(&corrupted, 1);
    file.close();

    auto read_result = storage->read_block(7);
    ASSERT_TRUE(read_result.is_error());
    EXPECT_EQ(read_result.error(), ErrorCode::BLOCK_CORRUPTED);
}

TEST_F(BlockStorageTest, RecoveryAfterRestart) {
    {
        auto result = BlockStorage::create(dir_, 1024 * 1024);
        ASSERT_TRUE(result.is_ok());
        auto storage = std::move(result.value());

        ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>{1, 2, 3}), ErrorCode::OK);
        ASSERT_EQ(storage->write_block(2, std::vector<uint8_t>{4, 5, 6, 7}), ErrorCode::OK);
    }  // storage destroyed here, simulating a process restart

    // Simulate a crash mid-write: a stray temp file left over on disk.
    std::ofstream stray(dir_ / "blocks" / "999.block.tmp");
    stray << "incomplete";
    stray.close();
    ASSERT_TRUE(std::filesystem::exists(dir_ / "blocks" / "999.block.tmp"));

    auto result2 = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result2.is_ok());
    auto storage2 = std::move(result2.value());

    EXPECT_TRUE(storage2->block_exists(1));
    EXPECT_TRUE(storage2->block_exists(2));

    auto read1 = storage2->read_block(1);
    ASSERT_TRUE(read1.is_ok());
    EXPECT_EQ(read1.value(), (std::vector<uint8_t>{1, 2, 3}));

    EXPECT_EQ(storage2->used_space(), 3u + 4u);
    EXPECT_FALSE(std::filesystem::exists(dir_ / "blocks" / "999.block.tmp"));
}

TEST_F(BlockStorageTest, ReadNonexistentBlock) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    auto read_result = storage->read_block(12345);
    ASSERT_TRUE(read_result.is_error());
    EXPECT_EQ(read_result.error(), ErrorCode::BLOCK_NOT_FOUND);
}

TEST_F(BlockStorageTest, DeleteIsIdempotent) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    std::vector<uint8_t> data = {9, 9, 9};
    ASSERT_EQ(storage->write_block(5, data), ErrorCode::OK);
    ASSERT_TRUE(storage->block_exists(5));

    EXPECT_EQ(storage->delete_block(5), ErrorCode::OK);
    EXPECT_FALSE(storage->block_exists(5));

    // Deleting again must still succeed.
    EXPECT_EQ(storage->delete_block(5), ErrorCode::OK);

    auto read_result = storage->read_block(5);
    ASSERT_TRUE(read_result.is_error());
    EXPECT_EQ(read_result.error(), ErrorCode::BLOCK_NOT_FOUND);
}

TEST_F(BlockStorageTest, StorageFullRejectsOversizedWrite) {
    auto result = BlockStorage::create(dir_, 10);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    std::vector<uint8_t> data(11, 0x01);
    EXPECT_EQ(storage->write_block(1, data), ErrorCode::STORAGE_FULL);
    EXPECT_FALSE(storage->block_exists(1));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "blocks" / "1.block"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "blocks" / "1.block.tmp"));
}

// Faza 10: write_block_at() (see docs/design.md §14).

TEST_F(BlockStorageTest, WriteBlockAtOverwritesWithinExistingContent) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>{1, 2, 3, 4, 5}), ErrorCode::OK);
    ASSERT_EQ(storage->write_block_at(1, 1, std::vector<uint8_t>{9, 9}), ErrorCode::OK);

    auto read_result = storage->read_block(1);
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), (std::vector<uint8_t>{1, 9, 9, 4, 5}));
}

TEST_F(BlockStorageTest, WriteBlockAtExtendsWithoutGap) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>{1, 2, 3}), ErrorCode::OK);
    ASSERT_EQ(storage->write_block_at(1, 3, std::vector<uint8_t>{4, 5}), ErrorCode::OK);

    auto read_result = storage->read_block(1);
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), (std::vector<uint8_t>{1, 2, 3, 4, 5}));
}

TEST_F(BlockStorageTest, WriteBlockAtExtendsWithGapZeroFills) {
    // A later-offset append can legitimately land before an earlier one
    // physically arrives (see docs/design.md §14) — write_block_at() must
    // not require offset == current length.
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>{1, 2}), ErrorCode::OK);
    ASSERT_EQ(storage->write_block_at(1, 5, std::vector<uint8_t>{9, 9}), ErrorCode::OK);

    auto read_result = storage->read_block(1);
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), (std::vector<uint8_t>{1, 2, 0, 0, 0, 9, 9}));
}

TEST_F(BlockStorageTest, WriteBlockAtOnMissingBlockFails) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    EXPECT_EQ(storage->write_block_at(404, 0, std::vector<uint8_t>{1}), ErrorCode::BLOCK_NOT_FOUND);
}

TEST_F(BlockStorageTest, WriteBlockAtRejectsWriteBeyondBlockSize) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>{1}), ErrorCode::OK);
    EXPECT_EQ(storage->write_block_at(1, BLOCK_SIZE - 1, std::vector<uint8_t>{1, 2}),
              ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(storage->write_block_at(1, BLOCK_SIZE + 1, std::vector<uint8_t>{1}),
              ErrorCode::INVALID_REQUEST);
}

TEST_F(BlockStorageTest, ConcurrentReadsAndWritesStayConsistent) {
    auto result = BlockStorage::create(dir_, 10 * 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    constexpr int kBlockCount = 20;
    std::vector<std::thread> threads;
    for (int i = 0; i < kBlockCount; ++i) {
        threads.emplace_back([&storage, i]() {
            std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
            storage->write_block(static_cast<BlockId>(i), data);
            storage->block_exists(static_cast<BlockId>(i));
            storage->read_block(static_cast<BlockId>(i));
        });
    }
    for (auto& t : threads) t.join();

    uint64_t expected_used = 0;
    for (int i = 0; i < kBlockCount; ++i) {
        EXPECT_TRUE(storage->block_exists(static_cast<BlockId>(i)));
        expected_used += 100;
    }
    EXPECT_EQ(storage->used_space(), expected_used);
}

// Faza 12.2: block_version / get_block_info / enumerate_blocks (see
// docs/design.md §16).

TEST_F(BlockStorageTest, WriteBlockPersistsVersionAndGetBlockInfoReturnsIt) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    std::vector<uint8_t> data = {1, 2, 3};
    ASSERT_EQ(storage->write_block(1, data, /*block_version=*/7), ErrorCode::OK);

    auto info = storage->get_block_info(1);
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().size, data.size());
    EXPECT_EQ(info.value().version, 7u);
}

TEST_F(BlockStorageTest, GetBlockInfoOnMissingBlockFails) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    EXPECT_EQ(storage->get_block_info(999).error(), ErrorCode::BLOCK_NOT_FOUND);
}

TEST_F(BlockStorageTest, WriteBlockAtPreservesPassedVersion) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>{1, 2}, /*block_version=*/1),
              ErrorCode::OK);
    ASSERT_EQ(storage->write_block_at(1, 2, std::vector<uint8_t>{3}, /*block_version=*/2),
              ErrorCode::OK);

    auto info = storage->get_block_info(1);
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().version, 2u);
}

// Faza 13: read_block_if_changed (see docs/design.md §17).

TEST_F(BlockStorageTest, ReadBlockIfChangedReturnsNotModifiedWhenVersionMatches) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    ASSERT_EQ(storage->write_block(1, {1, 2, 3}, /*block_version=*/9), ErrorCode::OK);

    auto conditional = storage->read_block_if_changed(1, /*known_version=*/9);
    ASSERT_TRUE(conditional.is_ok());
    EXPECT_TRUE(conditional.value().not_modified);
    EXPECT_EQ(conditional.value().version, 9u);
    EXPECT_TRUE(conditional.value().data.empty());
}

TEST_F(BlockStorageTest, ReadBlockIfChangedReturnsFullDataWhenVersionDiffers) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    std::vector<uint8_t> data = {4, 5, 6};
    ASSERT_EQ(storage->write_block(1, data, /*block_version=*/9), ErrorCode::OK);

    auto conditional = storage->read_block_if_changed(1, /*known_version=*/3);
    ASSERT_TRUE(conditional.is_ok());
    EXPECT_FALSE(conditional.value().not_modified);
    EXPECT_EQ(conditional.value().version, 9u);
    EXPECT_EQ(conditional.value().data, data);
}

TEST_F(BlockStorageTest, ReadBlockIfChangedWithZeroKnownVersionAlwaysReturnsFullData) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    // known_version=0 is the "nothing cached" sentinel — must never be
    // treated as a match even if a block somehow carried version 0.
    std::vector<uint8_t> data = {7};
    ASSERT_EQ(storage->write_block(1, data, /*block_version=*/0), ErrorCode::OK);

    auto conditional = storage->read_block_if_changed(1, /*known_version=*/0);
    ASSERT_TRUE(conditional.is_ok());
    EXPECT_FALSE(conditional.value().not_modified);
    EXPECT_EQ(conditional.value().data, data);
}

TEST_F(BlockStorageTest, ReadBlockIfChangedOnMissingBlockFails) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    EXPECT_EQ(storage->read_block_if_changed(999, 0).error(), ErrorCode::BLOCK_NOT_FOUND);
}

// cpp-reviewer regression (see docs/design.md §17): the version and data
// returned together must always describe the SAME on-disk state, even
// under concurrent writers. Before the fix, handle_read_block() checked
// the version and read the data as two separate lock acquisitions, so a
// write landing in between could pair an old version with new data (or
// vice versa). Each writer thread tags its data with its own version
// number so any observed (version, data) pair can be checked for a match.
TEST_F(BlockStorageTest, ReadBlockIfChangedNeverMixesVersionFromOneWriteWithDataFromAnother) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    ASSERT_EQ(storage->write_block(1, {0}, /*block_version=*/0), ErrorCode::OK);

    constexpr int kIterations = 500;
    std::atomic<bool> mismatch{false};

    std::thread writer([&]() {
        for (uint64_t v = 1; v <= kIterations; ++v) {
            storage->write_block(1, std::vector<uint8_t>(50, static_cast<uint8_t>(v)), v);
        }
    });
    std::thread reader([&]() {
        for (int i = 0; i < kIterations; ++i) {
            auto conditional = storage->read_block_if_changed(1, /*known_version=*/0);
            if (!conditional.is_ok() || conditional.value().not_modified) continue;
            const auto& data = conditional.value().data;
            uint64_t version = conditional.value().version;
            for (uint8_t byte : data) {
                if (byte != static_cast<uint8_t>(version)) {
                    mismatch = true;
                }
            }
        }
    });
    writer.join();
    reader.join();

    EXPECT_FALSE(mismatch) << "returned data did not match its own returned version";
}

TEST_F(BlockStorageTest, EnumerateBlocksReturnsEveryBlockWithCorrectInfo) {
    auto result = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>{1, 2}, /*block_version=*/3),
              ErrorCode::OK);
    ASSERT_EQ(storage->write_block(2, std::vector<uint8_t>{1, 2, 3}, /*block_version=*/5),
              ErrorCode::OK);

    auto blocks = storage->enumerate_blocks();
    ASSERT_EQ(blocks.size(), 2u);
    std::sort(blocks.begin(), blocks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    EXPECT_EQ(blocks[0].first, 1u);
    EXPECT_EQ(blocks[0].second.version, 3u);
    EXPECT_EQ(blocks[1].first, 2u);
    EXPECT_EQ(blocks[1].second.version, 5u);
}

TEST_F(BlockStorageTest, RecoverAfterRestartReadsPersistedVersion) {
    {
        auto result = BlockStorage::create(dir_, 1024 * 1024);
        ASSERT_TRUE(result.is_ok());
        ASSERT_EQ(result.value()->write_block(1, std::vector<uint8_t>{9, 9}, /*block_version=*/42),
                  ErrorCode::OK);
    }

    auto reopened = BlockStorage::create(dir_, 1024 * 1024);
    ASSERT_TRUE(reopened.is_ok());
    auto info = reopened.value()->get_block_info(1);
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().version, 42u);
}
