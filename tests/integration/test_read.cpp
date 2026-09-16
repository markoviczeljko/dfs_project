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

// Full CREATE -> WRITE -> READ round-trip through a real Client against an
// in-process Master + 3 StorageNodes cluster (RF=3, see docs/design.md
// §11/§13).
class ReadIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_read_integration_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        client_.reset();
        for (auto& node : storage_nodes_) node->stop();
        if (master_) master_->stop();
        std::filesystem::remove_all(dir_);
    }

    // Master must start before any StorageNode — a StorageNode registers
    // with Master synchronously at startup and fails if it isn't reachable
    // (see docs/design.md §13).
    void start_cluster(uint16_t master_port, uint16_t storage_port1, uint16_t storage_port2,
                        uint16_t storage_port3, uint64_t block_size = BLOCK_SIZE) {
        Master::Config master_config{master_port, dir_ / "master_metadata"};
        auto master_result = Master::create(master_config);
        ASSERT_TRUE(master_result.is_ok());
        master_ = std::move(master_result.value());
        ASSERT_EQ(master_->start(), ErrorCode::OK);

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
            ASSERT_TRUE(storage_result.is_ok());
            storage_nodes_.push_back(std::move(storage_result.value()));
            ASSERT_EQ(storage_nodes_.back()->start(), ErrorCode::OK);
        }

        auto client_result = Client::create(Client::Config{"127.0.0.1", master_port, block_size});
        ASSERT_TRUE(client_result.is_ok());
        client_ = std::move(client_result.value());
    }

    std::filesystem::path dir_;
    std::vector<std::unique_ptr<StorageNode>> storage_nodes_;
    std::unique_ptr<Master> master_;
    std::unique_ptr<Client> client_;
};

TEST_F(ReadIntegrationTest, WriteThenReadReturnsSameBytes) {
    start_cluster(40200, 40201, 40202, 40203);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data = {10, 20, 30, 40, 50};
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(ReadIntegrationTest, ReadEmptyFileReturnsEmptyVector) {
    start_cluster(40210, 40211, 40212, 40213);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_TRUE(read_result.value().empty());
}

TEST_F(ReadIntegrationTest, ReadNonexistentFileFails) {
    start_cluster(40220, 40221, 40222, 40223);

    auto read_result = client_->read("/nope.txt");
    ASSERT_TRUE(read_result.is_error());
    EXPECT_EQ(read_result.error(), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(ReadIntegrationTest, OverwriteReplacesContent) {
    start_cluster(40230, 40231, 40232, 40233);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    ASSERT_EQ(client_->write("/a.txt", {1, 2, 3}), ErrorCode::OK);
    ASSERT_EQ(client_->write("/a.txt", {9, 9}), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), std::vector<uint8_t>({9, 9}));
}

TEST_F(ReadIntegrationTest, ReadMultiBlockFileReturnsSameBytesInOrder) {
    constexpr uint64_t kSmallBlockSize = 4096;
    start_cluster(40240, 40241, 40242, 40243, kSmallBlockSize);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(static_cast<size_t>(kSmallBlockSize) * 2 + 500);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i % 251);
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(ReadIntegrationTest, ReadAfterOverwriteWithFewerBlocksReturnsNewContentOnly) {
    constexpr uint64_t kSmallBlockSize = 4096;
    start_cluster(40250, 40251, 40252, 40253, kSmallBlockSize);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> big_data(static_cast<size_t>(kSmallBlockSize) * 3, 5);
    ASSERT_EQ(client_->write("/a.txt", big_data), ErrorCode::OK);

    std::vector<uint8_t> small_data = {1, 2, 3, 4};
    ASSERT_EQ(client_->write("/a.txt", small_data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), small_data);
}

TEST_F(ReadIntegrationTest, ReadFallsBackToSurvivingReplicaAfterOneNodeStops) {
    // Faza 9's own "definition of done" node-failure check: kill one of 3
    // Storage Node processes (here, stop one in-process StorageNode
    // object), confirm READ still succeeds via client-side replica
    // fallback — Master never learns the node died (no heartbeat/failure
    // detection until Faza 12, see docs/design.md §13).
    start_cluster(40260, 40261, 40262, 40263);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data = {7, 7, 7, 7};
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    storage_nodes_[0]->stop();

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}
