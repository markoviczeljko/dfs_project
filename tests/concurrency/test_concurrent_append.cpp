#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

#include "storage/block_storage.h"

// Faza 15 (§11.41): 100 threads each append a distinct 100-byte chunk to the
// SAME block via write_block_at(), mirroring how NamespaceManager hands out
// non-overlapping offsets in the real system (BlockStorage only has to
// honor whatever offset it's given). The per-block exclusive lock taken by
// write_block_at() for the whole read-modify-write must make this behave
// exactly as if the 100 appends ran one after another: final size exactly
// 10,000 bytes, every chunk intact, no lost writes, no zero-gaps.
TEST(ConcurrentAppendTest, HundredThreadsAppendingSameBlockLoseNoData) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dfs_concurrent_append_test";
    std::filesystem::remove_all(dir);

    auto result = BlockStorage::create(dir, 10 * 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    constexpr int kThreads = 100;
    constexpr int kChunkSize = 100;
    ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>{}), ErrorCode::OK);

    std::atomic<uint64_t> next_offset{0};
    // Threads don't claim offsets in loop order — record which offset each
    // thread actually got so the assertion checks the right segment.
    std::vector<uint64_t> assigned_offset(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            uint64_t offset = next_offset.fetch_add(kChunkSize);
            assigned_offset[static_cast<size_t>(i)] = offset;
            std::vector<uint8_t> chunk(kChunkSize, static_cast<uint8_t>(i));
            ErrorCode err = storage->write_block_at(1, offset, chunk);
            EXPECT_EQ(err, ErrorCode::OK);
        });
    }
    for (auto& t : threads) t.join();

    auto info = storage->get_block_info(1);
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().size, static_cast<uint64_t>(kThreads * kChunkSize));

    auto read_result = storage->read_block(1);
    ASSERT_TRUE(read_result.is_ok());
    const std::vector<uint8_t>& data = read_result.value();
    ASSERT_EQ(data.size(), static_cast<size_t>(kThreads * kChunkSize));

    // Every kChunkSize-byte segment must be uniformly its owning thread's
    // own marker byte — a lost write would leave a segment mixed or zeroed.
    for (int i = 0; i < kThreads; ++i) {
        uint8_t expected = static_cast<uint8_t>(i);
        uint64_t offset = assigned_offset[static_cast<size_t>(i)];
        for (int j = 0; j < kChunkSize; ++j) {
            ASSERT_EQ(data[static_cast<size_t>(offset) + static_cast<size_t>(j)], expected)
                << "thread " << i << "'s segment at offset " << offset << " byte " << j << " corrupted";
        }
    }

    std::filesystem::remove_all(dir);
}
