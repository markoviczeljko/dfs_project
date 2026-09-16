#include <gtest/gtest.h>

#include <endian.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "client/client.h"
#include "common/config.h"
#include "master/master.h"
#include "network/connection.h"
#include "network/socket.h"
#include "storage/storage_node.h"

// Full cluster: Master + 3 StorageNodes (RF=3, see docs/design.md §13),
// driven by a real Client (not raw sockets) — the shape every later
// integration test reuses (see docs/design.md §11).
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

TEST_F(WriteIntegrationTest, WriteToUncreatedFileFails) {
    start_cluster(40100, 40101, 40102, 40103);

    std::vector<uint8_t> data = {1, 2, 3};
    EXPECT_EQ(client_->write("/a.txt", data), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(WriteIntegrationTest, CreateThenWriteSucceeds) {
    start_cluster(40110, 40111, 40112, 40113);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    EXPECT_EQ(client_->write("/a.txt", data), ErrorCode::OK);
}

TEST_F(WriteIntegrationTest, WriteBeyondBlockSizeSplitsIntoMultipleBlocks) {
    // Faza 8: a write larger than one block now succeeds by splitting into
    // multiple blocks (see docs/design.md §12), instead of the Faza 7
    // fail-fast this test used to assert.
    constexpr uint64_t kSmallBlockSize = 4096;
    start_cluster(40120, 40121, 40122, 40123, kSmallBlockSize);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(static_cast<size_t>(kSmallBlockSize) + 1);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(WriteIntegrationTest, WriteExactlyOneBlockProducesSingleBlock) {
    constexpr uint64_t kSmallBlockSize = 4096;
    start_cluster(40130, 40131, 40132, 40133, kSmallBlockSize);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(static_cast<size_t>(kSmallBlockSize), 7);
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(WriteIntegrationTest, WriteExactlyTwoBlocksProducesNoPhantomThirdBlock) {
    // Boundary case for the ceil() chunking math: an exact multiple of
    // block_size must produce exactly N blocks, never a phantom empty
    // trailing block (see docs/design.md §12).
    constexpr uint64_t kSmallBlockSize = 4096;
    start_cluster(40140, 40141, 40142, 40143, kSmallBlockSize);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(static_cast<size_t>(kSmallBlockSize) * 2);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(WriteIntegrationTest, WriteTwoFullBlocksPlusPartialThirdBlock) {
    constexpr uint64_t kSmallBlockSize = 4096;
    start_cluster(40150, 40151, 40152, 40153, kSmallBlockSize);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(static_cast<size_t>(kSmallBlockSize) * 2 + 123);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(WriteIntegrationTest, OverwriteMultiBlockFileWithFewerBlocksReplacesBlockList) {
    constexpr uint64_t kSmallBlockSize = 4096;
    start_cluster(40160, 40161, 40162, 40163, kSmallBlockSize);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> big_data(static_cast<size_t>(kSmallBlockSize) * 3, 1);
    ASSERT_EQ(client_->write("/a.txt", big_data), ErrorCode::OK);

    std::vector<uint8_t> small_data = {9, 9, 9};
    ASSERT_EQ(client_->write("/a.txt", small_data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), small_data);
}

namespace {
void put_u64(std::vector<std::byte>& buf, uint64_t value) {
    uint64_t be = htobe64(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}
}  // namespace

TEST_F(WriteIntegrationTest, WriteReplicatesBlockToAllThreeNodes) {
    // Literal "Definicija gotovog" check from IMPLEMENTATION_PLAN.md's Faza
    // 9 section: after a write, every block must be physically present on
    // all 3 Storage Nodes.
    start_cluster(40170, 40171, 40172, 40173);

    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    ASSERT_EQ(client_->write("/a.txt", data), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    // We don't know the block_id directly, but READ succeeding means it
    // was fetched from at least one replica — read the same block from
    // each of the 3 nodes' own ports directly and confirm all 3 have it.
    // The block_id is discoverable via a raw READ_FILE, mirroring the
    // pattern test_master.cpp/test_storage_node.cpp already use.
    Message meta_request{MessageType::READ_FILE, 1, std::vector<std::byte>()};
    {
        const std::string path = "/a.txt";
        meta_request.payload.assign(reinterpret_cast<const std::byte*>(path.data()),
                                     reinterpret_cast<const std::byte*>(path.data()) + path.size());
    }
    auto master_socket_result = Socket::create();
    ASSERT_TRUE(master_socket_result.is_ok());
    Socket master_socket = std::move(master_socket_result.value());
    ASSERT_EQ(master_socket.connect("127.0.0.1", 40170), ErrorCode::OK);
    TCPConnection master_connection(std::move(master_socket));
    ASSERT_TRUE(master_connection.sendMessage(meta_request));
    auto meta_response = master_connection.receiveMessage();
    ASSERT_TRUE(meta_response.has_value());
    ASSERT_EQ(meta_response->type, MessageType::ACK);

    uint64_t block_id;
    ASSERT_GE(meta_response->payload.size(), 28 + sizeof(block_id));
    std::memcpy(&block_id, meta_response->payload.data() + 28, sizeof(block_id));
    block_id = be64toh(block_id);

    for (uint16_t port : {40171, 40172, 40173}) {
        auto socket_result = Socket::create();
        ASSERT_TRUE(socket_result.is_ok());
        Socket socket = std::move(socket_result.value());
        ASSERT_EQ(socket.connect("127.0.0.1", port), ErrorCode::OK);
        TCPConnection connection(std::move(socket));

        std::vector<std::byte> payload;
        put_u64(payload, block_id);
        put_u64(payload, /*known_version=*/0);
        ASSERT_TRUE(connection.sendMessage(Message{MessageType::READ_BLOCK, 1, std::move(payload)}));
        auto resp = connection.receiveMessage();
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->type, MessageType::READ_OK) << "block missing on port " << port;
    }
}
