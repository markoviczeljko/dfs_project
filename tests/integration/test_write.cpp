#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "client/client.h"
#include "common/config.h"
#include "master/master.h"
#include "storage/storage_node.h"

// Full three-component in-process cluster: Master + one StorageNode, driven
// by a real Client (not raw sockets) — the shape every later integration
// test reuses (see docs/design.md §11).
class WriteIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_write_integration_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        client_.reset();
        if (master_) master_->stop();
        if (storage_node_) storage_node_->stop();
        std::filesystem::remove_all(dir_);
    }

    void start_cluster(uint16_t master_port, uint16_t storage_port) {
        StorageNode::Config storage_config{1, storage_port, dir_, 1024ULL * 1024 * 1024};
        auto storage_result = StorageNode::create(storage_config);
        ASSERT_TRUE(storage_result.is_ok());
        storage_node_ = std::move(storage_result.value());
        ASSERT_EQ(storage_node_->start(), ErrorCode::OK);

        Master::Config master_config{master_port, "127.0.0.1", storage_port};
        auto master_result = Master::create(master_config);
        ASSERT_TRUE(master_result.is_ok());
        master_ = std::move(master_result.value());
        ASSERT_EQ(master_->start(), ErrorCode::OK);

        auto client_result = Client::create(Client::Config{"127.0.0.1", master_port});
        ASSERT_TRUE(client_result.is_ok());
        client_ = std::move(client_result.value());
    }

    std::filesystem::path dir_;
    std::unique_ptr<StorageNode> storage_node_;
    std::unique_ptr<Master> master_;
    std::unique_ptr<Client> client_;
};

TEST_F(WriteIntegrationTest, WriteToUncreatedFileFails) {
    start_cluster(39003, 39004);

    std::vector<uint8_t> data = {1, 2, 3};
    EXPECT_EQ(client_->write("/a.txt", data), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(WriteIntegrationTest, CreateThenWriteSucceeds) {
    start_cluster(39005, 39006);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    EXPECT_EQ(client_->write("/a.txt", data), ErrorCode::OK);
}

TEST_F(WriteIntegrationTest, WriteBeyondBlockSizeFailsWithoutContactingMaster) {
    start_cluster(39007, 39008);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(static_cast<size_t>(BLOCK_SIZE) + 1, 0);
    EXPECT_EQ(client_->write("/a.txt", data), ErrorCode::INVALID_REQUEST);
}
