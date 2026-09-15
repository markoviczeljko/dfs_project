#include <gtest/gtest.h>

#include <endian.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
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

std::string get_string(const std::byte* src, uint32_t length) {
    return std::string(reinterpret_cast<const char*>(src), length);
}

Message make_write_commit_request(uint64_t request_id, const std::string& path,
                                   const std::vector<BlockId>& block_ids, uint64_t size) {
    // Faza 8 wire format: [u32 path_len][path][u64 size][u32 block_count]
    // [block_count x u64 block_id] (see docs/design.md §12).
    std::vector<std::byte> payload;
    put_u32(payload, static_cast<uint32_t>(path.size()));
    const auto* path_bytes = reinterpret_cast<const std::byte*>(path.data());
    payload.insert(payload.end(), path_bytes, path_bytes + path.size());
    put_u64(payload, size);
    put_u32(payload, static_cast<uint32_t>(block_ids.size()));
    for (BlockId block_id : block_ids) {
        put_u64(payload, block_id);
    }
    return Message{MessageType::WRITE_COMMIT, request_id, std::move(payload)};
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

    void start_master(uint16_t port, const std::string& storage_host = "127.0.0.1",
                       uint16_t storage_port = 9101) {
        Master::Config config{port, storage_host, storage_port};
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

TEST_F(MasterTest, WriteFileRemainsOutOfScope) {
    // READ_FILE is repurposed in Faza 7 (see below) — WRITE_FILE still has
    // no defined wire flow anywhere in the design notes and stays rejected.
    start_master(38009);
    TCPConnection client = connect_client(38009);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::WRITE_FILE, 1, "/a.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, AllocateBlockReturnsIdAndConfiguredStorageNode) {
    start_master(38015, "127.0.0.1", 9101);
    TCPConnection client = connect_client(38015);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    ASSERT_GE(resp->payload.size(), sizeof(uint64_t) + sizeof(uint32_t));
    BlockId block_id = get_u64(resp->payload.data());
    uint32_t host_length = get_u32(resp->payload.data() + 8);
    std::string host = get_string(resp->payload.data() + 12, host_length);
    uint16_t port = get_u16(resp->payload.data() + 12 + host_length);
    EXPECT_GT(block_id, 0u);
    EXPECT_EQ(host, "127.0.0.1");
    EXPECT_EQ(port, 9101);
}

TEST_F(MasterTest, AllocateBlockOnNonexistentFileFails) {
    start_master(38016);
    TCPConnection client = connect_client(38016);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 1, "/nope.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(MasterTest, ConcurrentAllocateBlockProducesUniqueIds) {
    // allocate_block()'s next_block_id_ counter is brand new in this phase
    // and had no concurrency test anywhere — this exercises the same claim
    // as ConcurrentCreateOnSamePathYieldsExactlyOneSuccess above, but for
    // the new counter instead of the file map.
    constexpr uint16_t port = 38022;
    start_master(port);

    TCPConnection setup_client = connect_client(port);
    ASSERT_TRUE(setup_client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(setup_client.receiveMessage().has_value());

    constexpr int kClients = 8;
    std::vector<std::thread> threads;
    std::mutex ids_mutex;
    std::set<BlockId> block_ids;

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([port, i, &ids_mutex, &block_ids]() {
            TCPConnection client = connect_client(port);
            ASSERT_TRUE(client.sendMessage(
                make_path_request(MessageType::ALLOCATE_BLOCK, static_cast<uint64_t>(i), "/a.txt")));
            auto resp = client.receiveMessage();
            ASSERT_TRUE(resp.has_value());
            ASSERT_EQ(resp->type, MessageType::ACK);
            BlockId block_id = get_u64(resp->payload.data());

            std::lock_guard<std::mutex> lock(ids_mutex);
            block_ids.insert(block_id);
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(block_ids.size(), static_cast<size_t>(kClients));
}

TEST_F(MasterTest, WriteCommitUpdatesMetadata) {
    start_master(38017);
    TCPConnection client = connect_client(38017);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    ASSERT_EQ(alloc_resp->type, MessageType::ACK);
    BlockId block_id = get_u64(alloc_resp->payload.data());

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(3, "/a.txt", {block_id}, /*size=*/1234)));
    auto commit_resp = client.receiveMessage();
    ASSERT_TRUE(commit_resp.has_value());
    EXPECT_EQ(commit_resp->type, MessageType::ACK);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::STAT_FILE, 4, "/a.txt")));
    auto stat_resp = client.receiveMessage();
    ASSERT_TRUE(stat_resp.has_value());
    EXPECT_EQ(stat_resp->type, MessageType::ACK);

    uint64_t size = get_u64(stat_resp->payload.data() + 8);
    uint32_t block_count = get_u32(stat_resp->payload.data() + 24);
    BlockId committed_block_id = get_u64(stat_resp->payload.data() + 28);
    EXPECT_EQ(size, 1234u);
    EXPECT_EQ(block_count, 1u);
    EXPECT_EQ(committed_block_id, block_id);
}

TEST_F(MasterTest, WriteCommitOnNonexistentFileFails) {
    start_master(38018);
    TCPConnection client = connect_client(38018);

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(1, "/nope.txt", {/*block_id=*/1}, /*size=*/1)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(MasterTest, WriteCommitWithoutPriorAllocationFails) {
    // Regression test: commit_write() must reject a block_id that was never
    // handed out by allocate_block() for this path — otherwise a client
    // could point a file at an unrelated, already-written block belonging
    // to someone else (see docs/design.md §11).
    start_master(38023);
    TCPConnection client = connect_client(38023);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(2, "/a.txt", {/*block_id=*/999}, /*size=*/1)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, WriteCommitAcceptsAnyPreviouslyAllocatedIdForThisPath) {
    // Faza 8: a multi-chunk write needs several ALLOCATE_BLOCK calls to
    // stay simultaneously valid until one final WRITE_COMMIT, so — unlike
    // Faza 7 — an older allocation for a path is no longer invalidated the
    // moment a newer one for the same path is issued (see docs/design.md
    // §12). This replaces the old WriteCommitWithStaleBlockIdFails test,
    // whose property (newer allocation supersedes older one) is
    // deliberately dropped.
    start_master(38024);
    TCPConnection client = connect_client(38024);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto first_alloc = client.receiveMessage();
    ASSERT_TRUE(first_alloc.has_value());
    BlockId first_block_id = get_u64(first_alloc->payload.data());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 3, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(4, "/a.txt", {first_block_id}, /*size=*/1)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
}

TEST_F(MasterTest, WriteCommitWithBlockIdFromDifferentPathFails) {
    // Cross-path anti-theft regression: a block_id allocated for one path
    // must never be committable under a different path (see
    // docs/design.md §12).
    start_master(38025);
    TCPConnection client = connect_client(38025);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 2, "/b.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 3, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    BlockId block_id = get_u64(alloc_resp->payload.data());

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(4, "/b.txt", {block_id}, /*size=*/1)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, WriteCommitWithMultipleBlocksSucceedsAndPreservesOrder) {
    start_master(38026);
    TCPConnection client = connect_client(38026);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    std::vector<BlockId> block_ids;
    for (uint64_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(client.sendMessage(
            make_path_request(MessageType::ALLOCATE_BLOCK, 2 + i, "/a.txt")));
        auto resp = client.receiveMessage();
        ASSERT_TRUE(resp.has_value());
        block_ids.push_back(get_u64(resp->payload.data()));
    }

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(5, "/a.txt", block_ids, /*size=*/150)));
    auto commit_resp = client.receiveMessage();
    ASSERT_TRUE(commit_resp.has_value());
    EXPECT_EQ(commit_resp->type, MessageType::ACK);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::STAT_FILE, 6, "/a.txt")));
    auto stat_resp = client.receiveMessage();
    ASSERT_TRUE(stat_resp.has_value());
    ASSERT_EQ(stat_resp->type, MessageType::ACK);

    uint32_t block_count = get_u32(stat_resp->payload.data() + 24);
    ASSERT_EQ(block_count, 3u);
    for (size_t i = 0; i < block_ids.size(); ++i) {
        BlockId returned = get_u64(stat_resp->payload.data() + 28 + i * sizeof(uint64_t));
        EXPECT_EQ(returned, block_ids[i]);
    }
}

TEST_F(MasterTest, WriteCommitSubsetOfPendingBlocksSucceedsAndDiscardsUnusedIds) {
    // Documents the abandoned-write-attempt scenario: allocating more
    // blocks than are ultimately committed must not leave the path
    // permanently unwritable (see docs/design.md §12).
    start_master(38027);
    TCPConnection client = connect_client(38027);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    std::vector<BlockId> allocated;
    for (uint64_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(client.sendMessage(
            make_path_request(MessageType::ALLOCATE_BLOCK, 2 + i, "/a.txt")));
        auto resp = client.receiveMessage();
        ASSERT_TRUE(resp.has_value());
        allocated.push_back(get_u64(resp->payload.data()));
    }

    // Commit only the first 2 of the 3 allocated blocks — the 3rd is
    // silently discarded (orphaned), not an error.
    std::vector<BlockId> committed(allocated.begin(), allocated.begin() + 2);
    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(5, "/a.txt", committed, /*size=*/100)));
    auto first_commit = client.receiveMessage();
    ASSERT_TRUE(first_commit.has_value());
    EXPECT_EQ(first_commit->type, MessageType::ACK);

    // A second, fully independent legitimate write to the same path must
    // still work cleanly afterward.
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 6, "/a.txt")));
    auto second_alloc = client.receiveMessage();
    ASSERT_TRUE(second_alloc.has_value());
    BlockId second_block_id = get_u64(second_alloc->payload.data());

    ASSERT_TRUE(client.sendMessage(
        make_write_commit_request(7, "/a.txt", {second_block_id}, /*size=*/50)));
    auto second_commit = client.receiveMessage();
    ASSERT_TRUE(second_commit.has_value());
    EXPECT_EQ(second_commit->type, MessageType::ACK);
}

TEST_F(MasterTest, WriteCommitWithUnallocatedIdAmongOtherwiseValidIdsFails) {
    start_master(38028);
    TCPConnection client = connect_client(38028);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    std::vector<BlockId> allocated;
    for (uint64_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(client.sendMessage(
            make_path_request(MessageType::ALLOCATE_BLOCK, 2 + i, "/a.txt")));
        auto resp = client.receiveMessage();
        ASSERT_TRUE(resp.has_value());
        allocated.push_back(get_u64(resp->payload.data()));
    }
    allocated.push_back(99999);  // never allocated

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(4, "/a.txt", allocated, /*size=*/1)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, ReadFileOnEmptyFileReturnsZeroBlocks) {
    start_master(38019);
    TCPConnection client = connect_client(38019);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::READ_FILE, 2, "/a.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    uint32_t block_count = get_u32(resp->payload.data() + 24);
    EXPECT_EQ(block_count, 0u);
}

TEST_F(MasterTest, ReadFileReturnsMetadataWithBlockLocation) {
    start_master(38020, "127.0.0.1", 9101);
    TCPConnection client = connect_client(38020);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    ASSERT_EQ(alloc_resp->type, MessageType::ACK);
    BlockId block_id = get_u64(alloc_resp->payload.data());

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(3, "/a.txt", {block_id}, /*size=*/9)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::READ_FILE, 4, "/a.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    uint32_t block_count = get_u32(resp->payload.data() + 24);
    ASSERT_EQ(block_count, 1u);
    BlockId returned_block_id = get_u64(resp->payload.data() + 28);
    uint32_t host_length = get_u32(resp->payload.data() + 36);
    std::string host = get_string(resp->payload.data() + 40, host_length);
    uint16_t port = get_u16(resp->payload.data() + 40 + host_length);
    EXPECT_EQ(returned_block_id, block_id);
    EXPECT_EQ(host, "127.0.0.1");
    EXPECT_EQ(port, 9101);
}

TEST_F(MasterTest, ReadFileOnNonexistentFileFails) {
    start_master(38021);
    TCPConnection client = connect_client(38021);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::READ_FILE, 1, "/nope.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::FILE_NOT_FOUND);
}
