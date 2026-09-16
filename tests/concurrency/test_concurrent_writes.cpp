#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

#include "storage/block_storage.h"

// Faza 15 (§11.43): one writer repeatedly overwrites a block with a buffer
// filled entirely with an incrementing marker byte, while several readers
// concurrently read_block() it. Each returned buffer must be internally
// consistent — every byte equal to the buffer's own first byte — proving
// the per-block exclusive-write/shared-read lock never lets a reader
// observe a torn, half-old/half-new block (same property already covered
// for read_block_if_changed by test_block_storage.cpp's
// ReadBlockIfChangedNeverMixesVersionFromOneWriteWithDataFromAnother, now
// proven for plain read_block()/write_block() too).
TEST(ConcurrentWritesTest, ReadersNeverObserveATornBlockDuringConcurrentWrites) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dfs_concurrent_writes_test";
    std::filesystem::remove_all(dir);

    auto result = BlockStorage::create(dir, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    // Kept small deliberately: write_block() fsyncs the block file AND the
    // containing directory on every call, so this is a real-disk-bound
    // loop, not a CPU-bound one — a few dozen fsync'd writes racing several
    // spin-reading threads is already enough opportunity to catch a torn
    // read if per-block locking were broken.
    constexpr int kIterations = 20;
    constexpr int kReaderThreads = 8;
    ASSERT_EQ(storage->write_block(1, std::vector<uint8_t>(64, 0)), ErrorCode::OK);

    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};

    std::thread writer([&]() {
        for (int v = 1; v <= kIterations; ++v) {
            storage->write_block(1, std::vector<uint8_t>(64, static_cast<uint8_t>(v)));
        }
        stop = true;
    });

    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);
    for (int r = 0; r < kReaderThreads; ++r) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                auto read_result = storage->read_block(1);
                if (!read_result.is_ok()) continue;
                const auto& data = read_result.value();
                if (data.empty()) continue;
                uint8_t first = data[0];
                for (uint8_t byte : data) {
                    if (byte != first) {
                        torn = true;
                    }
                }
            }
        });
    }

    writer.join();
    for (auto& t : readers) t.join();

    EXPECT_FALSE(torn) << "a reader observed a torn (half-old/half-new) block";

    std::filesystem::remove_all(dir);
}

// Cross-block invariant: BlockStorage::used_space_ accounting must stay
// correct under concurrent writes to DIFFERENT blocks, where no single
// write exceeds capacity but their sum would — exactly the case
// try_apply_space_delta()'s check-and-reserve exists for (see
// block_storage.h). Exactly one thread's write must be rejected with
// STORAGE_FULL, and used_space() must never exceed capacity().
TEST(ConcurrentWritesTest, ConcurrentWritesToDifferentBlocksRespectSharedCapacity) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dfs_concurrent_capacity_test";
    std::filesystem::remove_all(dir);

    constexpr uint64_t kCapacity = 300;
    constexpr int kBlockCount = 4;
    constexpr uint64_t kBlockPayload = 100;  // 4 * 100 = 400 > 300 capacity

    auto result = BlockStorage::create(dir, kCapacity);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    std::atomic<int> successes{0};
    std::atomic<int> rejections{0};
    std::vector<std::thread> threads;
    threads.reserve(kBlockCount);
    for (int i = 0; i < kBlockCount; ++i) {
        threads.emplace_back([&, i]() {
            std::vector<uint8_t> data(kBlockPayload, static_cast<uint8_t>(i));
            ErrorCode err = storage->write_block(static_cast<BlockId>(i), data);
            if (err == ErrorCode::OK) {
                successes.fetch_add(1);
            } else {
                EXPECT_EQ(err, ErrorCode::STORAGE_FULL);
                rejections.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(successes.load() + rejections.load(), kBlockCount);
    EXPECT_EQ(successes.load(), 3);  // exactly 3 of 4 fit within capacity 300
    EXPECT_LE(storage->used_space(), kCapacity);

    std::filesystem::remove_all(dir);
}

// cpp-reviewer regression (see docs/design.md §19): a write_block() that
// resolves its BlockState via get_or_create_block_state() and then loses a
// race to a concurrent delete_block() on the SAME id must never let its
// write land in a BlockState the map no longer references — the original
// bug (fixed by never erasing a BlockState from the map) let write_block()
// return OK while the freshly-written data became permanently invisible
// through the public API. Hammer write/delete on one id concurrently, then
// confirm every accessor still agrees on whether the block exists, and
// that a final deterministic write is fully visible afterward.
TEST(ConcurrentWritesTest, ConcurrentWriteAndDeleteOnSameIdNeverOrphansAWrite) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dfs_concurrent_write_delete_test";
    std::filesystem::remove_all(dir);

    auto result = BlockStorage::create(dir, 1024 * 1024);
    ASSERT_TRUE(result.is_ok());
    auto storage = std::move(result.value());

    constexpr BlockId kId = 1;
    constexpr int kIterations = 500;
    ASSERT_EQ(storage->write_block(kId, std::vector<uint8_t>{1, 2, 3}), ErrorCode::OK);

    std::thread writer([&]() {
        for (int v = 0; v < kIterations; ++v) {
            storage->write_block(kId, std::vector<uint8_t>(8, static_cast<uint8_t>(v)));
        }
    });
    std::thread deleter([&]() {
        for (int v = 0; v < kIterations; ++v) {
            storage->delete_block(kId);
        }
    });
    writer.join();
    deleter.join();

    // Every accessor must agree on whether the block exists — a mismatch
    // (e.g. read_block() succeeding while block_exists() says false) would
    // indicate the write/delete race left the internal state inconsistent.
    bool exists = storage->block_exists(kId);
    auto read_after_storm = storage->read_block(kId);
    auto info_after_storm = storage->get_block_info(kId);
    EXPECT_EQ(exists, read_after_storm.is_ok());
    EXPECT_EQ(exists, info_after_storm.is_ok());

    // The storage must still be fully functional afterward — no deadlock,
    // no corruption, and a fresh write must be visible through every
    // accessor exactly as it would be for a block that was never raced.
    std::vector<uint8_t> final_data = {9, 9, 9, 9};
    ASSERT_EQ(storage->write_block(kId, final_data, /*block_version=*/42), ErrorCode::OK);
    EXPECT_TRUE(storage->block_exists(kId));
    auto final_read = storage->read_block(kId);
    ASSERT_TRUE(final_read.is_ok());
    EXPECT_EQ(final_read.value(), final_data);
    auto final_info = storage->get_block_info(kId);
    ASSERT_TRUE(final_info.is_ok());
    EXPECT_EQ(final_info.value().version, 42u);

    std::filesystem::remove_all(dir);
}
