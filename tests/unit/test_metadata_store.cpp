#include <gtest/gtest.h>

#include <endian.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "master/metadata_store.h"

namespace {

// Same CRC-32 (IEEE 802.3, 0xEDB88320) as MetadataStore's own — duplicated
// here (matching this project's convention of duplicating small
// wire-format helpers per module) to construct a deliberately malicious
// but still CRC-valid snapshot for the reserve()-bounds regression test
// below.
uint32_t compute_crc32(const std::vector<std::byte>& data) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (std::byte b : data) {
        crc = table[(crc ^ static_cast<uint8_t>(b)) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void put_u16(std::vector<std::byte>& buf, uint16_t value) {
    uint16_t be = htobe16(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}

void put_u32(std::vector<std::byte>& buf, uint32_t value) {
    uint32_t be = htobe32(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}

void put_u64(std::vector<std::byte>& buf, uint64_t value) {
    uint64_t be = htobe64(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}

}  // namespace

class MetadataStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_metadata_store_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path snapshot_path() const { return dir_ / "metadata.snapshot"; }

    std::filesystem::path dir_;
};

TEST_F(MetadataStoreTest, LoadOnEmptyDirectoryReturnsEmptyState) {
    auto result = MetadataStore::create(dir_);
    ASSERT_TRUE(result.is_ok());
    auto store = std::move(result.value());

    auto load_result = store->load();
    ASSERT_TRUE(load_result.is_ok());
    const MetadataStore::LoadedState& state = load_result.value();
    EXPECT_TRUE(state.files.empty());
    EXPECT_TRUE(state.blocks.empty());
    EXPECT_TRUE(state.nodes.empty());
    EXPECT_EQ(state.next_file_id, 1u);
    EXPECT_EQ(state.next_block_id, 1u);
}

TEST_F(MetadataStoreTest, SaveThenLoadRoundTripsFilesBlocksAndNodes) {
    auto result = MetadataStore::create(dir_);
    ASSERT_TRUE(result.is_ok());
    auto store = std::move(result.value());

    std::vector<FileMetadata> files{
        FileMetadata{1, "/a.txt", 100, 123456789, {10, 11}},
        FileMetadata{2, "/data/b.txt", 0, 987654321, {}},
    };
    std::vector<std::pair<BlockId, BlockMetadata>> blocks{
        {10, BlockMetadata{10, 60, 60, 1, {1, 2, 3}, BlockState::COMMITTED}},
        {11, BlockMetadata{11, 40, 40, 2, {2, 3, 4}, BlockState::UNDER_REPLICATED}},
    };
    std::vector<StorageNodeInfo> nodes{
        StorageNodeInfo{1, "127.0.0.1", 9101, NodeState::HEALTHY, 1024, 512, {}},
        StorageNodeInfo{2, "127.0.0.1", 9102, NodeState::SUSPECTED, 2048, 2048, {}},
    };

    ASSERT_EQ(store->save(files, blocks, /*next_file_id=*/3, /*next_block_id=*/12, nodes), ErrorCode::OK);
    ASSERT_TRUE(std::filesystem::exists(snapshot_path()));

    auto load_result = store->load();
    ASSERT_TRUE(load_result.is_ok());
    const MetadataStore::LoadedState& state = load_result.value();

    EXPECT_EQ(state.next_file_id, 3u);
    EXPECT_EQ(state.next_block_id, 12u);

    ASSERT_EQ(state.files.size(), 2u);
    EXPECT_EQ(state.files[0].id, 1u);
    EXPECT_EQ(state.files[0].path, "/a.txt");
    EXPECT_EQ(state.files[0].size, 100u);
    EXPECT_EQ(state.files[0].modification_time, 123456789u);
    EXPECT_EQ(state.files[0].blocks, (std::vector<BlockId>{10, 11}));
    EXPECT_EQ(state.files[1].path, "/data/b.txt");
    EXPECT_TRUE(state.files[1].blocks.empty());

    ASSERT_EQ(state.blocks.size(), 2u);
    EXPECT_EQ(state.blocks[0].first, 10u);
    EXPECT_EQ(state.blocks[0].second.size, 60u);
    EXPECT_EQ(state.blocks[0].second.reserved_size, 60u);
    EXPECT_EQ(state.blocks[0].second.version, 1u);
    EXPECT_EQ(state.blocks[0].second.state, BlockState::COMMITTED);
    EXPECT_EQ(state.blocks[0].second.replicas, (std::vector<StorageNodeId>{1, 2, 3}));
    EXPECT_EQ(state.blocks[1].second.state, BlockState::UNDER_REPLICATED);

    ASSERT_EQ(state.nodes.size(), 2u);
    EXPECT_EQ(state.nodes[0].id, 1u);
    EXPECT_EQ(state.nodes[0].host, "127.0.0.1");
    EXPECT_EQ(state.nodes[0].port, 9101u);
    EXPECT_EQ(state.nodes[0].total_space, 1024u);
    EXPECT_EQ(state.nodes[0].free_space, 512u);
}

TEST_F(MetadataStoreTest, LoadDiscardsStrayTempFileWithoutAffectingCanonicalSnapshot) {
    auto result = MetadataStore::create(dir_);
    ASSERT_TRUE(result.is_ok());
    auto store = std::move(result.value());

    std::vector<FileMetadata> files{FileMetadata{1, "/a.txt", 5, 1, {}}};
    ASSERT_EQ(store->save(files, {}, 2, 1, {}), ErrorCode::OK);

    // Simulate a crash mid-write: a stray incomplete temp file left over
    // alongside the last successfully-published (renamed) snapshot — same
    // scenario as BlockStorage::RecoveryAfterRestart.
    std::ofstream stray(dir_ / "metadata.snapshot.tmp");
    stray << "incomplete";
    stray.close();
    ASSERT_TRUE(std::filesystem::exists(dir_ / "metadata.snapshot.tmp"));

    auto load_result = store->load();
    ASSERT_TRUE(load_result.is_ok());
    EXPECT_EQ(load_result.value().files.size(), 1u);
    EXPECT_EQ(load_result.value().files[0].path, "/a.txt");
    EXPECT_FALSE(std::filesystem::exists(dir_ / "metadata.snapshot.tmp"));
}

TEST_F(MetadataStoreTest, LoadOnCorruptedSnapshotReturnsEmptyStateNotError) {
    auto result = MetadataStore::create(dir_);
    ASSERT_TRUE(result.is_ok());
    auto store = std::move(result.value());

    std::vector<FileMetadata> files{FileMetadata{1, "/a.txt", 5, 1, {}}};
    ASSERT_EQ(store->save(files, {}, 2, 1, {}), ErrorCode::OK);

    // Corrupt one byte directly on disk, bypassing MetadataStore's own API
    // — the trailing CRC32 must catch this (see docs/design.md §16).
    std::fstream file(snapshot_path(), std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file.seekp(10);
    char corrupted = static_cast<char>(0xFF);
    file.write(&corrupted, 1);
    file.close();

    auto load_result = store->load();
    ASSERT_TRUE(load_result.is_ok());  // corrupted => treated as fresh, never a hard error
    EXPECT_TRUE(load_result.value().files.empty());
    EXPECT_EQ(load_result.value().next_file_id, 1u);
}

TEST_F(MetadataStoreTest, LoadWithHugeCountFieldDoesNotCrashOrThrow) {
    // Bugfix regression: .reserve(count) used to be called directly on an
    // unvalidated on-disk count field. A crafted (still CRC-valid — CRC32
    // isn't cryptographically secure) snapshot with an absurd file_count
    // and no actual entries after it used to throw
    // std::length_error/bad_alloc, an uncaught exception that would crash
    // Master at startup — directly contradicting load()'s own documented
    // "a corrupted snapshot never refuses to boot" contract (see
    // docs/design.md §16).
    auto result = MetadataStore::create(dir_);
    ASSERT_TRUE(result.is_ok());
    auto store = std::move(result.value());

    std::vector<std::byte> buf;
    put_u32(buf, 0x4D455453);   // magic "METS"
    put_u16(buf, 1);            // format_version
    put_u64(buf, 1);            // next_file_id
    put_u64(buf, 1);            // next_block_id
    put_u32(buf, 0xFFFFFFFFu);  // file_count — bogus, far beyond remaining bytes
    // (deliberately truncated here — no actual file entries follow)
    put_u32(buf, compute_crc32(buf));

    std::ofstream file(snapshot_path(), std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    file.close();

    auto load_result = store->load();
    ASSERT_TRUE(load_result.is_ok());  // must not throw or crash
    EXPECT_TRUE(load_result.value().files.empty());  // truncated mid-parse — discarded
}

TEST_F(MetadataStoreTest, MultipleSavesOverwritePreviousSnapshot) {
    auto result = MetadataStore::create(dir_);
    ASSERT_TRUE(result.is_ok());
    auto store = std::move(result.value());

    ASSERT_EQ(store->save({FileMetadata{1, "/first.txt", 1, 1, {}}}, {}, 2, 1, {}), ErrorCode::OK);
    ASSERT_EQ(store->save({FileMetadata{1, "/second.txt", 2, 2, {}}}, {}, 2, 1, {}), ErrorCode::OK);

    auto load_result = store->load();
    ASSERT_TRUE(load_result.is_ok());
    ASSERT_EQ(load_result.value().files.size(), 1u);
    EXPECT_EQ(load_result.value().files[0].path, "/second.txt");
}
