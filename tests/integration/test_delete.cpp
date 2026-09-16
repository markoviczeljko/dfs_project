#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "client/client.h"
#include "common/config.h"
#include "master/master.h"
#include "storage/storage_node.h"

// Faza 11 — DELETE / WRITE-overwrite orphan-block GC (see docs/design.md
// §15). Same cluster shape as AppendIntegrationTest: Master + 3
// StorageNodes, driven by a real Client.
class DeleteIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_delete_integration_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        clients_.clear();
        for (auto& node : storage_nodes_) node->stop();
        if (master_) master_->stop();
        std::filesystem::remove_all(dir_);
    }

    // Master must start before any StorageNode — see docs/design.md §13.
    Client& start_cluster(uint16_t master_port, uint16_t storage_port1, uint16_t storage_port2,
                           uint16_t storage_port3, uint64_t block_size = BLOCK_SIZE) {
        Master::Config master_config{master_port, dir_ / "master_metadata"};
        auto master_result = Master::create(master_config);
        EXPECT_TRUE(master_result.is_ok());
        master_ = std::move(master_result.value());
        EXPECT_EQ(master_->start(), ErrorCode::OK);
        master_port_ = master_port;

        std::array<uint16_t, 3> storage_ports = {storage_port1, storage_port2, storage_port3};
        for (size_t i = 0; i < storage_ports.size(); ++i) {
            StorageNode::Config storage_config{static_cast<StorageNodeId>(i + 1),
                                                storage_ports[i],
                                                dir_ / ("node" + std::to_string(i + 1)),
                                                1024ULL * 1024 * 1024,
                                                "127.0.0.1",
                                                "127.0.0.1",
                                                master_port};
            auto storage_result = StorageNode::create(storage_config);
            EXPECT_TRUE(storage_result.is_ok());
            storage_nodes_.push_back(std::move(storage_result.value()));
            EXPECT_EQ(storage_nodes_.back()->start(), ErrorCode::OK);
        }

        return new_client(block_size);
    }

    Client& new_client(uint64_t block_size = BLOCK_SIZE) {
        auto client_result = Client::create(Client::Config{"127.0.0.1", master_port_, block_size});
        EXPECT_TRUE(client_result.is_ok());
        clients_.push_back(std::move(client_result.value()));
        return *clients_.back();
    }

    // Direct on-disk check — node_index is 0-based (0 == storage_nodes_[0]),
    // matching how block files are physically laid out (see
    // BlockStorage::block_path, docs/design.md §15).
    bool block_exists_on_disk(size_t node_index, BlockId id) const {
        std::filesystem::path path = dir_ / ("node" + std::to_string(node_index + 1)) / "blocks" /
                                      (std::to_string(id) + ".block");
        return std::filesystem::exists(path);
    }

    std::filesystem::path dir_;
    std::vector<std::unique_ptr<StorageNode>> storage_nodes_;
    std::unique_ptr<Master> master_;
    std::vector<std::unique_ptr<Client>> clients_;
    uint16_t master_port_ = 0;
};

TEST_F(DeleteIntegrationTest, DeleteRemovesFileFromNamespace) {
    Client& client = start_cluster(40400, 40401, 40402, 40403);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data = {1, 2, 3};
    ASSERT_EQ(client.write("/a.txt", data), ErrorCode::OK);

    ASSERT_EQ(client.delete_file("/a.txt"), ErrorCode::OK);

    auto stat_result = client.stat("/a.txt");
    ASSERT_TRUE(stat_result.is_error());
    EXPECT_EQ(stat_result.error(), ErrorCode::FILE_NOT_FOUND);

    auto read_result = client.read("/a.txt");
    EXPECT_TRUE(read_result.is_error());
}

TEST_F(DeleteIntegrationTest, DeletePhysicallyRemovesBlocksFromAllThreeReplicasOnDisk) {
    constexpr uint64_t kSmallBlockSize = 16;
    Client& client = start_cluster(40410, 40411, 40412, 40413, kSmallBlockSize);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(static_cast<size_t>(kSmallBlockSize) * 2 + 5, 9);  // 3 blocks
    ASSERT_EQ(client.write("/a.txt", data), ErrorCode::OK);

    auto stat_result = client.stat("/a.txt");
    ASSERT_TRUE(stat_result.is_ok());
    std::vector<BlockId> blocks = stat_result.value().blocks;
    ASSERT_EQ(blocks.size(), 3u);
    for (BlockId id : blocks) {
        for (size_t node = 0; node < 3; ++node) {
            EXPECT_TRUE(block_exists_on_disk(node, id)) << "block " << id << " node " << node;
        }
    }

    ASSERT_EQ(client.delete_file("/a.txt"), ErrorCode::OK);

    for (BlockId id : blocks) {
        for (size_t node = 0; node < 3; ++node) {
            EXPECT_FALSE(block_exists_on_disk(node, id)) << "block " << id << " node " << node;
        }
    }
}

TEST_F(DeleteIntegrationTest, WriteOverwriteOrphansOldBlocksPhysicallyOnAllReplicas) {
    Client& client = start_cluster(40420, 40421, 40422, 40423);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> v1(100, 1);
    ASSERT_EQ(client.write("/a.txt", v1), ErrorCode::OK);

    auto stat1 = client.stat("/a.txt");
    ASSERT_TRUE(stat1.is_ok());
    ASSERT_EQ(stat1.value().blocks.size(), 1u);
    BlockId old_block = stat1.value().blocks[0];

    std::vector<uint8_t> v2(50, 2);
    ASSERT_EQ(client.write("/a.txt", v2), ErrorCode::OK);

    auto stat2 = client.stat("/a.txt");
    ASSERT_TRUE(stat2.is_ok());
    ASSERT_EQ(stat2.value().blocks.size(), 1u);
    BlockId new_block = stat2.value().blocks[0];
    ASSERT_NE(old_block, new_block);

    for (size_t node = 0; node < 3; ++node) {
        EXPECT_FALSE(block_exists_on_disk(node, old_block));
        EXPECT_TRUE(block_exists_on_disk(node, new_block));
    }

    auto read_result = client.read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), v2);
}

TEST_F(DeleteIntegrationTest, PartialWriteFailureLeavesOldVersionFullyReadable) {
    // Acceptance criterion 2 (see docs/design.md §15): a WRITE that fails
    // partway through must leave the OLD version fully readable — nothing
    // this phase changes about commit_write()'s ordering, but exercised
    // end-to-end against a real dead replica for the first time.
    constexpr uint64_t kSmallBlockSize = 16;
    Client& client = start_cluster(40430, 40431, 40432, 40433, kSmallBlockSize);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> v1(10, 1);
    ASSERT_EQ(client.write("/a.txt", v1), ErrorCode::OK);

    storage_nodes_[0]->stop();

    // 2 full blocks — write_chunk_to_replicas requires all 3 replicas to
    // ACK (strict 3/3, see docs/design.md §13), so this must fail as soon
    // as it reaches the dead node, before any WRITE_COMMIT is ever sent.
    std::vector<uint8_t> v2(static_cast<size_t>(kSmallBlockSize) * 2, 2);
    EXPECT_NE(client.write("/a.txt", v2), ErrorCode::OK);

    // READ still falls back to the 2 surviving replicas (see
    // docs/design.md §13), so the old, still-valid version is unaffected.
    auto read_result = client.read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), v1);
}

TEST_F(DeleteIntegrationTest, DeleteNonexistentFileFails) {
    Client& client = start_cluster(40440, 40441, 40442, 40443);
    EXPECT_EQ(client.delete_file("/nope.txt"), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(DeleteIntegrationTest, DeleteThenRecreateSamePathSucceeds) {
    Client& client = start_cluster(40450, 40451, 40452, 40453);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    ASSERT_EQ(client.delete_file("/a.txt"), ErrorCode::OK);
    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);

    auto stat_result = client.stat("/a.txt");
    ASSERT_TRUE(stat_result.is_ok());
    EXPECT_EQ(stat_result.value().size, 0u);
}
