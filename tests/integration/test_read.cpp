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

// Full CREATE -> WRITE -> READ round-trip through a real Client against an
// in-process Master + StorageNode cluster (see docs/design.md §11).
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
        if (master_) master_->stop();
        if (storage_node_) storage_node_->stop();
        std::filesystem::remove_all(dir_);
    }

    void start_cluster(uint16_t master_port, uint16_t storage_port,
                        uint64_t block_size = BLOCK_SIZE) {
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

        auto client_result =
            Client::create(Client::Config{"127.0.0.1", master_port, block_size});
        ASSERT_TRUE(client_result.is_ok());
        client_ = std::move(client_result.value());
    }

    std::filesystem::path dir_;
    std::unique_ptr<StorageNode> storage_node_;
    std::unique_ptr<Master> master_;
    std::unique_ptr<Client> client_;
};

TEST_F(ReadIntegrationTest, WriteThenReadReturnsSameBytes) {
    start_cluster(39009, 39010);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data = {10, 20, 30, 40, 50};
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(ReadIntegrationTest, ReadEmptyFileReturnsEmptyVector) {
    start_cluster(39011, 39012);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_TRUE(read_result.value().empty());
}

TEST_F(ReadIntegrationTest, ReadNonexistentFileFails) {
    start_cluster(39013, 39014);

    auto read_result = client_->read("/nope.txt");
    ASSERT_TRUE(read_result.is_error());
    EXPECT_EQ(read_result.error(), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(ReadIntegrationTest, OverwriteReplacesContent) {
    start_cluster(39015, 39016);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    ASSERT_EQ(client_->write("/a.txt", {1, 2, 3}), ErrorCode::OK);
    ASSERT_EQ(client_->write("/a.txt", {9, 9}), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), std::vector<uint8_t>({9, 9}));
}

TEST_F(ReadIntegrationTest, ReadMultiBlockFileReturnsSameBytesInOrder) {
    constexpr uint64_t kSmallBlockSize = 4096;
    start_cluster(39025, 39026, kSmallBlockSize);

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
    start_cluster(39027, 39028, kSmallBlockSize);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> big_data(static_cast<size_t>(kSmallBlockSize) * 3, 5);
    ASSERT_EQ(client_->write("/a.txt", big_data), ErrorCode::OK);

    std::vector<uint8_t> small_data = {1, 2, 3, 4};
    ASSERT_EQ(client_->write("/a.txt", small_data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), small_data);
}
