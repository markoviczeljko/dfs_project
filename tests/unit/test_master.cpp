#include <gtest/gtest.h>

#include <endian.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "master/master.h"
#include "network/connection.h"
#include "network/socket.h"

namespace {

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

Message make_path_request(MessageType type, uint64_t request_id, const std::string& path) {
    std::vector<std::byte> payload;
    const auto* bytes = reinterpret_cast<const std::byte*>(path.data());
    payload.insert(payload.end(), bytes, bytes + path.size());
    return Message{type, request_id, std::move(payload)};
}

ErrorCode error_from_payload(const std::vector<std::byte>& payload) {
    return static_cast<ErrorCode>(get_u16(payload.data()));
}

std::vector<std::string> parse_list_response(const std::vector<std::byte>& payload) {
    std::vector<std::string> entries;
    uint32_t count = get_u32(payload.data());
    size_t offset = sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t name_length = get_u32(payload.data() + offset);
        offset += sizeof(uint32_t);
        std::string name(reinterpret_cast<const char*>(payload.data() + offset), name_length);
        offset += name_length;
        entries.push_back(std::move(name));
    }
    return entries;
}

}  // namespace

class MasterTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (master_) master_->stop();
    }

    void start_master(uint16_t port) {
        Master::Config config{port};
        auto result = Master::create(config);
        ASSERT_TRUE(result.is_ok());
        master_ = std::move(result.value());
        ASSERT_EQ(master_->start(), ErrorCode::OK);
    }

    static TCPConnection connect_client(uint16_t port) {
        auto socket_result = Socket::create();
        EXPECT_TRUE(socket_result.is_ok());
        Socket socket = std::move(socket_result.value());
        EXPECT_EQ(socket.connect("127.0.0.1", port), ErrorCode::OK);
        return TCPConnection(std::move(socket));
    }

    std::unique_ptr<Master> master_;
};

TEST_F(MasterTest, CreateThenStatReturnsMetadata) {
    start_master(38001);
    TCPConnection client = connect_client(38001);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/data/test.txt")));
    auto create_resp = client.receiveMessage();
    ASSERT_TRUE(create_resp.has_value());
    EXPECT_EQ(create_resp->type, MessageType::ACK);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::STAT_FILE, 2, "/data/test.txt")));
    auto stat_resp = client.receiveMessage();
    ASSERT_TRUE(stat_resp.has_value());
    EXPECT_EQ(stat_resp->type, MessageType::ACK);

    ASSERT_GE(stat_resp->payload.size(), 3 * sizeof(uint64_t) + sizeof(uint32_t));
    uint64_t file_id = get_u64(stat_resp->payload.data());
    uint64_t size = get_u64(stat_resp->payload.data() + 8);
    uint32_t block_count = get_u32(stat_resp->payload.data() + 24);
    EXPECT_GT(file_id, 0u);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(block_count, 0u);  // no ALLOCATE_BLOCK yet — Faza 7
}

TEST_F(MasterTest, CreateOnExistingPathFails) {
    start_master(38002);
    TCPConnection client = connect_client(38002);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 2, "/a.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::FILE_ALREADY_EXISTS);
}

TEST_F(MasterTest, DeleteThenStatFails) {
    start_master(38003);
    TCPConnection client = connect_client(38003);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::DELETE_FILE, 2, "/a.txt")));
    auto delete_resp = client.receiveMessage();
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->type, MessageType::ACK);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::STAT_FILE, 3, "/a.txt")));
    auto stat_resp = client.receiveMessage();
    ASSERT_TRUE(stat_resp.has_value());
    EXPECT_EQ(stat_resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(stat_resp->payload), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(MasterTest, DeleteNonexistentFails) {
    start_master(38004);
    TCPConnection client = connect_client(38004);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::DELETE_FILE, 1, "/nope.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(MasterTest, StatNonexistentFails) {
    start_master(38005);
    TCPConnection client = connect_client(38005);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::STAT_FILE, 1, "/nope.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(MasterTest, ListFilesReturnsDirectChildren) {
    start_master(38006);
    TCPConnection client = connect_client(38006);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/data/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 2, "/data/b.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::LIST_FILES, 3, "/data")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    std::vector<std::string> entries = parse_list_response(resp->payload);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_NE(std::find(entries.begin(), entries.end(), "a.txt"), entries.end());
    EXPECT_NE(std::find(entries.begin(), entries.end(), "b.txt"), entries.end());
}

TEST_F(MasterTest, ListRootReturnsTopLevelEntries) {
    start_master(38007);
    TCPConnection client = connect_client(38007);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/top.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::LIST_FILES, 2, "/")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    std::vector<std::string> entries = parse_list_response(resp->payload);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0], "top.txt");
}

TEST_F(MasterTest, UnknownMessageTypeReturnsInvalidRequest) {
    start_master(38008);
    TCPConnection client = connect_client(38008);

    ASSERT_TRUE(client.sendMessage(Message{MessageType::HEARTBEAT, 1, {}}));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, EmptyPathPayloadReturnsInvalidRequest) {
    start_master(38010);
    TCPConnection client = connect_client(38010);

    // payload is genuinely zero bytes (not even "/") — exercises the
    // empty-vector -> std::string conversion path in bytes_to_string.
    ASSERT_TRUE(client.sendMessage(Message{MessageType::CREATE_FILE, 1, {}}));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, CreateFileOnRootReturnsInvalidRequest) {
    start_master(38011);
    TCPConnection client = connect_client(38011);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, PathNotStartingWithSlashReturnsInvalidRequest) {
    start_master(38012);
    TCPConnection client = connect_client(38012);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "relative.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, ListFilesAfterDeletingAllChildrenReturnsEmpty) {
    start_master(38013);
    TCPConnection client = connect_client(38013);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/data/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::DELETE_FILE, 2, "/data/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::LIST_FILES, 3, "/data")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
    EXPECT_TRUE(parse_list_response(resp->payload).empty());
}

TEST_F(MasterTest, ConcurrentCreateOnSamePathYieldsExactlyOneSuccess) {
    // Master itself adds no locking of its own — this exercises the claim
    // (docs/design.md §10) that NamespaceManager's internal shared_mutex is
    // sufficient to keep concurrent requests from the NetworkServer's
    // per-connection worker threads safe and mutually exclusive.
    constexpr uint16_t port = 38014;
    start_master(port);

    constexpr int kClients = 8;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> already_exists_count{0};

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([port, i, &success_count, &already_exists_count]() {
            TCPConnection client = connect_client(port);
            ASSERT_TRUE(client.sendMessage(
                make_path_request(MessageType::CREATE_FILE, static_cast<uint64_t>(i), "/race.txt")));
            auto resp = client.receiveMessage();
            ASSERT_TRUE(resp.has_value());
            if (resp->type == MessageType::ACK) {
                ++success_count;
            } else {
                ASSERT_EQ(error_from_payload(resp->payload), ErrorCode::FILE_ALREADY_EXISTS);
                ++already_exists_count;
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(success_count.load(), 1);
    EXPECT_EQ(already_exists_count.load(), kClients - 1);
}

TEST_F(MasterTest, ReadFileAndWriteFileAreOutOfScope) {
    start_master(38009);
    TCPConnection client = connect_client(38009);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::READ_FILE, 1, "/a.txt")));
    auto read_resp = client.receiveMessage();
    ASSERT_TRUE(read_resp.has_value());
    EXPECT_EQ(read_resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(read_resp->payload), ErrorCode::INVALID_REQUEST);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::WRITE_FILE, 2, "/a.txt")));
    auto write_resp = client.receiveMessage();
    ASSERT_TRUE(write_resp.has_value());
    EXPECT_EQ(write_resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(write_resp->payload), ErrorCode::INVALID_REQUEST);
}
