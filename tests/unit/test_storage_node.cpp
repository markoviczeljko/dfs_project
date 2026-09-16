#include <gtest/gtest.h>

#include <endian.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "master/master.h"
#include "network/connection.h"
#include "network/socket.h"
#include "storage/storage_node.h"

namespace {

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

uint16_t get_u16(const std::byte* src) {
    uint16_t be;
    std::memcpy(&be, src, sizeof(be));
    return be16toh(be);
}

uint32_t get_u32(const std::byte* src) {
    uint32_t be;
    std::memcpy(&be, src, sizeof(be));
    return be32toh(be);
}

uint64_t get_u64(const std::byte* src) {
    uint64_t be;
    std::memcpy(&be, src, sizeof(be));
    return be64toh(be);
}

// Faza 12.2 wire format: [u64 operation_id][u64 block_id][u64 offset]
// [u64 block_version][u32 data_size][data] (see docs/design.md §16).
// operation_id defaults to request_id — every call site here already uses
// a distinct request_id per message, which is a fine (if not the "real")
// source of uniqueness for these tests.
Message make_write_request(uint64_t request_id, BlockId block_id, const std::vector<uint8_t>& data,
                            uint64_t offset = 0, uint64_t block_version = 0,
                            uint64_t operation_id = 0) {
    std::vector<std::byte> payload;
    put_u64(payload, operation_id != 0 ? operation_id : request_id);
    put_u64(payload, block_id);
    put_u64(payload, offset);
    put_u64(payload, block_version);
    put_u32(payload, static_cast<uint32_t>(data.size()));
    const auto* data_bytes = reinterpret_cast<const std::byte*>(data.data());
    payload.insert(payload.end(), data_bytes, data_bytes + data.size());
    return Message{MessageType::WRITE_BLOCK, request_id, std::move(payload)};
}

Message make_block_id_request(MessageType type, uint64_t request_id, BlockId block_id) {
    std::vector<std::byte> payload;
    put_u64(payload, block_id);
    return Message{type, request_id, std::move(payload)};
}

ErrorCode error_from_payload(const std::vector<std::byte>& payload) {
    return static_cast<ErrorCode>(get_u16(payload.data()));
}

}  // namespace

class StorageNodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_storage_node_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        if (node_) node_->stop();
        if (master_) master_->stop();
        std::filesystem::remove_all(dir_);
    }

    // Faza 9: StorageNode::start() now registers with Master synchronously
    // and fails if it's unreachable, so this fixture needs a real
    // in-process Master too — even though these tests only exercise
    // WRITE_BLOCK/READ_BLOCK/DELETE_BLOCK, which never touch Master (see
    // docs/design.md §13). master_port is derived from the storage port
    // (+100) to stay collision-free with the existing per-test port
    // convention below without a separate port table.
    void start_node(uint16_t port, uint64_t capacity_bytes = 1024 * 1024) {
        uint16_t master_port = static_cast<uint16_t>(port + 100);
        Master::Config master_config{master_port};
        auto master_result = Master::create(master_config);
        ASSERT_TRUE(master_result.is_ok());
        master_ = std::move(master_result.value());
        ASSERT_EQ(master_->start(), ErrorCode::OK);

        StorageNode::Config config{1, port, dir_, capacity_bytes, "127.0.0.1", "127.0.0.1",
                                    master_port};
        auto result = StorageNode::create(config);
        ASSERT_TRUE(result.is_ok());
        node_ = std::move(result.value());
        ASSERT_EQ(node_->start(), ErrorCode::OK);
    }

    static TCPConnection connect_client(uint16_t port) {
        auto socket_result = Socket::create();
        EXPECT_TRUE(socket_result.is_ok());
        Socket socket = std::move(socket_result.value());
        EXPECT_EQ(socket.connect("127.0.0.1", port), ErrorCode::OK);
        return TCPConnection(std::move(socket));
    }

    std::filesystem::path dir_;
    std::unique_ptr<Master> master_;
    std::unique_ptr<StorageNode> node_;
};

TEST_F(StorageNodeTest, WriteThenReadReturnsSameBytes) {
    start_node(37001);
    TCPConnection client = connect_client(37001);

    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    ASSERT_TRUE(client.sendMessage(make_write_request(100, 1, data)));
    auto write_resp = client.receiveMessage();
    ASSERT_TRUE(write_resp.has_value());
    EXPECT_EQ(write_resp->type, MessageType::ACK);
    EXPECT_EQ(write_resp->request_id, 100u);

    ASSERT_TRUE(client.sendMessage(make_block_id_request(MessageType::READ_BLOCK, 101, 1)));
    auto read_resp = client.receiveMessage();
    ASSERT_TRUE(read_resp.has_value());
    EXPECT_EQ(read_resp->type, MessageType::ACK);
    EXPECT_EQ(read_resp->request_id, 101u);

    ASSERT_GE(read_resp->payload.size(), sizeof(uint64_t) + sizeof(uint32_t));
    BlockId returned_id = get_u64(read_resp->payload.data());
    uint32_t returned_size = get_u32(read_resp->payload.data() + sizeof(uint64_t));
    EXPECT_EQ(returned_id, 1u);
    ASSERT_EQ(read_resp->payload.size(), sizeof(uint64_t) + sizeof(uint32_t) + returned_size);

    std::vector<uint8_t> returned_data(returned_size);
    std::memcpy(returned_data.data(), read_resp->payload.data() + sizeof(uint64_t) + sizeof(uint32_t),
                returned_size);
    EXPECT_EQ(returned_data, data);
}

TEST_F(StorageNodeTest, DeleteThenReadReturnsBlockNotFound) {
    start_node(37002);
    TCPConnection client = connect_client(37002);

    std::vector<uint8_t> data = {9, 9};
    ASSERT_TRUE(client.sendMessage(make_write_request(1, 5, data)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_block_id_request(MessageType::DELETE_BLOCK, 2, 5)));
    auto delete_resp = client.receiveMessage();
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->type, MessageType::ACK);

    ASSERT_TRUE(client.sendMessage(make_block_id_request(MessageType::READ_BLOCK, 3, 5)));
    auto read_resp = client.receiveMessage();
    ASSERT_TRUE(read_resp.has_value());
    EXPECT_EQ(read_resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(read_resp->payload), ErrorCode::BLOCK_NOT_FOUND);
}

TEST_F(StorageNodeTest, DeleteIsIdempotentOnMissingBlock) {
    start_node(37003);
    TCPConnection client = connect_client(37003);

    ASSERT_TRUE(client.sendMessage(make_block_id_request(MessageType::DELETE_BLOCK, 1, 777)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
}

TEST_F(StorageNodeTest, ReadNonexistentBlockReturnsBlockNotFound) {
    start_node(37004);
    TCPConnection client = connect_client(37004);

    ASSERT_TRUE(client.sendMessage(make_block_id_request(MessageType::READ_BLOCK, 1, 42)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::BLOCK_NOT_FOUND);
}

TEST_F(StorageNodeTest, WriteBeyondCapacityReturnsStorageFull) {
    start_node(37005, /*capacity_bytes=*/4);
    TCPConnection client = connect_client(37005);

    std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};  // 8 bytes > 4-byte capacity
    ASSERT_TRUE(client.sendMessage(make_write_request(1, 1, data)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::STORAGE_FULL);
}

TEST_F(StorageNodeTest, UnknownMessageTypeReturnsInvalidRequest) {
    start_node(37006);
    TCPConnection client = connect_client(37006);

    ASSERT_TRUE(client.sendMessage(Message{MessageType::HEARTBEAT, 1, {}}));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(StorageNodeTest, WriteBlockWithMismatchedDataSizeReturnsInvalidRequest) {
    start_node(37007);
    TCPConnection client = connect_client(37007);

    std::vector<std::byte> payload;
    put_u64(payload, 999);  // operation_id
    put_u64(payload, 1);    // block_id
    put_u64(payload, 0);    // offset
    put_u64(payload, 0);    // block_version
    put_u32(payload, 100);  // claims 100 bytes but sends none
    ASSERT_TRUE(client.sendMessage(Message{MessageType::WRITE_BLOCK, 1, payload}));

    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}
