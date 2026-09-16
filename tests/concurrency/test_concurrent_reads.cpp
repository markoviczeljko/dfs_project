#include <gtest/gtest.h>

#include <filesystem>
#include <thread>
#include <vector>

#include "storage/block_storage.h"

// Faza 15 (§11.42): 50 threads concurrently read_block() the same,
// never-mutated block. Under per-block shared_lock reads, every thread must
// see byte-identical data and the same checksum/version — there is nothing
// racing a pure-read workload, but this proves shared_lock concurrency
// doesn't corrupt anything and is TSan-clean.
TEST(ConcurrentReadsTest, FiftyThreadsReadingSameBlockAllSeeIdenticalData) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dfs_concurrent_reads_test";
    std::filesystem::remove_all(dir);

    auto result = BlockStorage::create(dir, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    std::vector<uint8_t> original(4096);
    for (size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<uint8_t>(i % 256);
    }
    ASSERT_EQ(storage->write_block(1, original, /*block_version=*/7), ErrorCode::OK);

    constexpr int kThreads = 50;
    std::vector<std::thread> threads;
    // NOT std::vector<bool> — it packs multiple logical elements into one
    // underlying word, so distinct-index writes from different threads
    // race on the same byte (a real TSan finding, but in this test
    // harness, not in BlockStorage). uint8_t gives each thread its own,
    // genuinely independent memory location.
    std::vector<uint8_t> matched(kThreads, 0);
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            auto read_result = storage->read_block(1);
            auto info = storage->get_block_info(1);
            if (read_result.is_ok() && info.is_ok() && read_result.value() == original &&
                info.value().version == 7u) {
                matched[static_cast<size_t>(i)] = 1;
            }
        });
    }
    for (auto& t : threads) t.join();

    for (int i = 0; i < kThreads; ++i) {
        EXPECT_EQ(matched[static_cast<size_t>(i)], 1) << "thread " << i << " saw inconsistent data";
    }

    std::filesystem::remove_all(dir);
}
