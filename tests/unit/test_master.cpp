#include <gtest/gtest.h>

#include <endian.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "master/master.h"
#include "network/connection.h"
#include "network/network_server.h"
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

std::string get_string(const std::byte* src, uint32_t length) {
    return std::string(reinterpret_cast<const char*>(src), length);
}

Message make_write_commit_request(uint64_t request_id, const std::string& path,
                                   const std::vector<BlockId>& block_ids, uint64_t size,
                                   uint64_t last_block_size = 0) {
    // Faza 8 wire format: [u32 path_len][path][u64 size][u32 block_count]
    // [block_count x u64 block_id] (see docs/design.md §12), extended in
    // Faza 10 with a trailing [u64 last_block_size] (see docs/design.md
    // §14). None of these tests assert on BlockMetadata.size, so a default
    // of 0 keeps every existing call site unchanged.
    std::vector<std::byte> payload;
    put_u32(payload, static_cast<uint32_t>(path.size()));
    const auto* path_bytes = reinterpret_cast<const std::byte*>(path.data());
    payload.insert(payload.end(), path_bytes, path_bytes + path.size());
    put_u64(payload, size);
    put_u32(payload, static_cast<uint32_t>(block_ids.size()));
    for (BlockId block_id : block_ids) {
        put_u64(payload, block_id);
    }
    put_u64(payload, last_block_size);
    return Message{MessageType::WRITE_COMMIT, request_id, std::move(payload)};
}

// Faza 18 wire format: [u32 path_len][path][u32 block_count]
// [block_count x u64 block_id] (see docs/design_faza18_abandon_blocks.md).
Message make_abandon_blocks_request(uint64_t request_id, const std::string& path,
                                     const std::vector<BlockId>& block_ids) {
    std::vector<std::byte> payload;
    put_u32(payload, static_cast<uint32_t>(path.size()));
    const auto* path_bytes = reinterpret_cast<const std::byte*>(path.data());
    payload.insert(payload.end(), path_bytes, path_bytes + path.size());
    put_u32(payload, static_cast<uint32_t>(block_ids.size()));
    for (BlockId block_id : block_ids) {
        put_u64(payload, block_id);
    }
    return Message{MessageType::ABANDON_BLOCKS, request_id, std::move(payload)};
}

Message make_path_request(MessageType type, uint64_t request_id, const std::string& path) {
    std::vector<std::byte> payload;
    const auto* bytes = reinterpret_cast<const std::byte*>(path.data());
    payload.insert(payload.end(), bytes, bytes + path.size());
    return Message{type, request_id, std::move(payload)};
}

// Faza 9 wire format: [u64 node_id][u32 host_len][host][u16 port]
// [u64 capacity_bytes] (see docs/design.md §13).
Message make_register_node_request(uint64_t request_id, StorageNodeId node_id,
                                    const std::string& host, uint16_t port,
                                    uint64_t capacity_bytes) {
    std::vector<std::byte> payload;
    put_u64(payload, node_id);
    put_u32(payload, static_cast<uint32_t>(host.size()));
    const auto* host_bytes = reinterpret_cast<const std::byte*>(host.data());
    payload.insert(payload.end(), host_bytes, host_bytes + host.size());
    put_u16(payload, port);
    put_u64(payload, capacity_bytes);
    return Message{MessageType::REGISTER_NODE, request_id, std::move(payload)};
}

// Faza 12.1 wire format: [u64 node_id] (see docs/design.md §16).
Message make_heartbeat_request(uint64_t request_id, StorageNodeId node_id) {
    std::vector<std::byte> payload;
    put_u64(payload, node_id);
    return Message{MessageType::HEARTBEAT, request_id, std::move(payload)};
}

// Faza 12.3 wire format: [u64 node_id][u32 block_count]
// {[u64 block_id][u64 version][u32 checksum]} x block_count (see
// docs/design.md §16).
struct InventoryEntry {
    BlockId block_id;
    uint64_t version;
    uint32_t checksum;
};

Message make_node_inventory_request(uint64_t request_id, StorageNodeId node_id,
                                     const std::vector<InventoryEntry>& entries) {
    std::vector<std::byte> payload;
    put_u64(payload, node_id);
    put_u32(payload, static_cast<uint32_t>(entries.size()));
    for (const InventoryEntry& entry : entries) {
        put_u64(payload, entry.block_id);
        put_u64(payload, entry.version);
        put_u32(payload, entry.checksum);
    }
    return Message{MessageType::NODE_INVENTORY, request_id, std::move(payload)};
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

// Registers 3 nodes (ids 1-3, ports 19101-19103) over the given client
// connection — the minimum RF=3 needs for ALLOCATE_BLOCK to succeed (see
// docs/design.md §13). Uses request ids in the 900s to stay visually
// distinct from a test's own low, sequential ids (harmless either way —
// Master never deduplicates by request_id).
void register_three_nodes(TCPConnection& client) {
    for (uint64_t i = 1; i <= 3; ++i) {
        uint16_t port = static_cast<uint16_t>(19100 + i);
        ASSERT_TRUE(client.sendMessage(
            make_register_node_request(900 + i, i, "127.0.0.1", port, /*capacity_bytes=*/1024)));
        auto resp = client.receiveMessage();
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->type, MessageType::ACK);
    }
}

// Faza 11: a fake Storage Node that just records the block ids of every
// DELETE_BLOCK it receives and ACKs them, so gc_dispatch()'s behavior can be
// observed directly (see docs/design.md §15).
struct RecordingNode {
    std::mutex mutex;
    std::vector<BlockId> deleted_block_ids;
    // Faza 12.3: block ids named in any COPY_BLOCK this node receives — the
    // 2nd u64 field of COPY_BLOCK's payload (see
    // StorageNode::handle_copy_block's wire format comment). Lets
    // RecoveryManager::handle_stale_replica() tests observe which node
    // Master picked as the resync source, without this fake node needing
    // to actually forward anything on to a destination (see docs/design.md
    // §16).
    std::vector<BlockId> copy_block_ids;
    std::unique_ptr<NetworkServer> server;

    static std::unique_ptr<RecordingNode> start(uint16_t port) {
        auto node = std::make_unique<RecordingNode>();
        RecordingNode* self = node.get();
        auto result = NetworkServer::create(port, [self](const Message& request) -> Message {
            if (request.type == MessageType::DELETE_BLOCK &&
                request.payload.size() == sizeof(uint64_t)) {
                BlockId block_id = get_u64(request.payload.data());
                std::lock_guard<std::mutex> lock(self->mutex);
                self->deleted_block_ids.push_back(block_id);
            } else if (request.type == MessageType::COPY_BLOCK &&
                       request.payload.size() >= 2 * sizeof(uint64_t)) {
                BlockId block_id = get_u64(request.payload.data() + sizeof(uint64_t));
                std::lock_guard<std::mutex> lock(self->mutex);
                self->copy_block_ids.push_back(block_id);
            }
            return Message{MessageType::ACK, request.request_id, request.payload};
        });
        EXPECT_TRUE(result.is_ok());
        node->server = std::move(result.value());
        EXPECT_EQ(node->server->start(), ErrorCode::OK);
        return node;
    }

    bool received(BlockId block_id) {
        std::lock_guard<std::mutex> lock(mutex);
        return std::find(deleted_block_ids.begin(), deleted_block_ids.end(), block_id) !=
               deleted_block_ids.end();
    }

    bool received_copy_block(BlockId block_id) {
        std::lock_guard<std::mutex> lock(mutex);
        return std::find(copy_block_ids.begin(), copy_block_ids.end(), block_id) !=
               copy_block_ids.end();
    }
};

// A fake Storage Node that accepts the connection but never replies within
// sleep_ms — regression coverage for send_delete_block()'s I/O timeout (see
// docs/design.md §15).
struct HangingNode {
    std::unique_ptr<NetworkServer> server;

    static std::unique_ptr<HangingNode> start(uint16_t port, int sleep_ms) {
        auto node = std::make_unique<HangingNode>();
        auto result = NetworkServer::create(port, [sleep_ms](const Message& request) -> Message {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            return Message{MessageType::ACK, request.request_id, {}};
        });
        EXPECT_TRUE(result.is_ok());
        node->server = std::move(result.value());
        EXPECT_EQ(node->server->start(), ErrorCode::OK);
        return node;
    }
};

// Registers 3 RecordingNodes (ids 1-3) at the given ports and REGISTER_NODEs
// them with master over client — like register_three_nodes(), but backed by
// real listening servers so DELETE_BLOCK dispatch can be observed.
std::vector<std::unique_ptr<RecordingNode>> register_three_recording_nodes(
    TCPConnection& client, uint16_t port1, uint16_t port2, uint16_t port3) {
    std::vector<std::unique_ptr<RecordingNode>> nodes;
    nodes.push_back(RecordingNode::start(port1));
    nodes.push_back(RecordingNode::start(port2));
    nodes.push_back(RecordingNode::start(port3));

    uint16_t ports[3] = {port1, port2, port3};
    for (uint64_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(client.sendMessage(make_register_node_request(
            800 + i, i + 1, "127.0.0.1", ports[i], /*capacity_bytes=*/1024)));
        auto resp = client.receiveMessage();
        EXPECT_TRUE(resp.has_value());
        EXPECT_EQ(resp->type, MessageType::ACK);
    }
    return nodes;
}

}  // namespace

class MasterTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_master_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        if (master_) master_->stop();
        std::filesystem::remove_all(dir_);
    }

    void start_master(uint16_t port) {
        Master::Config config{port, dir_ / "master_metadata"};
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

    std::filesystem::path dir_;
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
    // REPLICATE_BLOCK stays a reserved-but-unhandled enum value (see
    // docs/design.md §16) — HEARTBEAT is handled as of Faza 12.1, see the
    // dedicated tests below.
    start_master(38008);
    TCPConnection client = connect_client(38008);

    ASSERT_TRUE(client.sendMessage(Message{MessageType::REPLICATE_BLOCK, 1, {}}));
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

TEST_F(MasterTest, RegisterNodeReturnsAck) {
    start_master(38029);
    TCPConnection client = connect_client(38029);

    ASSERT_TRUE(client.sendMessage(
        make_register_node_request(1, /*node_id=*/1, "127.0.0.1", 9101, /*capacity_bytes=*/1024)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
}

TEST_F(MasterTest, RegisterNodeTwiceWithSameIdSucceeds) {
    // Idempotent upsert — a node re-registering (e.g. a manual restart
    // during development) must not be rejected (see docs/design.md §13).
    start_master(38030);
    TCPConnection client = connect_client(38030);

    ASSERT_TRUE(client.sendMessage(
        make_register_node_request(1, /*node_id=*/1, "127.0.0.1", 9101, /*capacity_bytes=*/1024)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(
        make_register_node_request(2, /*node_id=*/1, "127.0.0.1", 9999, /*capacity_bytes=*/2048)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
}

TEST_F(MasterTest, AllocateBlockWithFewerThanThreeRegisteredNodesReturnsNodeNotFound) {
    start_master(38031);
    TCPConnection client = connect_client(38031);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    // 0 registered nodes.
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto resp0 = client.receiveMessage();
    ASSERT_TRUE(resp0.has_value());
    EXPECT_EQ(resp0->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp0->payload), ErrorCode::NODE_NOT_FOUND);

    // 1 registered node.
    ASSERT_TRUE(client.sendMessage(
        make_register_node_request(3, /*node_id=*/1, "127.0.0.1", 19101, /*capacity_bytes=*/1024)));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 4, "/a.txt")));
    auto resp1 = client.receiveMessage();
    ASSERT_TRUE(resp1.has_value());
    EXPECT_EQ(resp1->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp1->payload), ErrorCode::NODE_NOT_FOUND);

    // 2 registered nodes — still short of RF=3.
    ASSERT_TRUE(client.sendMessage(
        make_register_node_request(5, /*node_id=*/2, "127.0.0.1", 19102, /*capacity_bytes=*/1024)));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 6, "/a.txt")));
    auto resp2 = client.receiveMessage();
    ASSERT_TRUE(resp2.has_value());
    EXPECT_EQ(resp2->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp2->payload), ErrorCode::NODE_NOT_FOUND);
}

TEST_F(MasterTest, AllocateBlockReturnsThreeDistinctReplicaLocations) {
    start_master(38015);
    TCPConnection client = connect_client(38015);
    register_three_nodes(client);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    ASSERT_GE(resp->payload.size(), sizeof(uint64_t) + sizeof(uint32_t));
    BlockId block_id = get_u64(resp->payload.data());
    EXPECT_GT(block_id, 0u);
    uint32_t replica_count = get_u32(resp->payload.data() + sizeof(uint64_t));
    ASSERT_EQ(replica_count, 3u);

    size_t offset = sizeof(uint64_t) + sizeof(uint32_t);
    std::set<uint16_t> ports;
    for (uint32_t i = 0; i < replica_count; ++i) {
        uint32_t host_length = get_u32(resp->payload.data() + offset);
        offset += sizeof(uint32_t);
        std::string host = get_string(resp->payload.data() + offset, host_length);
        offset += host_length;
        uint16_t port = get_u16(resp->payload.data() + offset);
        offset += sizeof(uint16_t);
        EXPECT_EQ(host, "127.0.0.1");
        ports.insert(port);
    }
    EXPECT_EQ(ports.size(), 3u);

    // Faza 12.3: trailing [u64 version] — always 1 for a freshly allocated
    // block (see docs/design.md §16).
    ASSERT_EQ(resp->payload.size(), offset + sizeof(uint64_t));
    EXPECT_EQ(get_u64(resp->payload.data() + offset), 1u);
    offset += sizeof(uint64_t);
    EXPECT_EQ(resp->payload.size(), offset);
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
    register_three_nodes(setup_client);
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
    register_three_nodes(client);

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
    //
    // Port 38024 is durably occupied for the rest of this sandbox session
    // by a long-lived outbound ESTABLISHED connection (confirmed via
    // /proc/net/tcp — a real, diagnosed cause, not a superstition; same
    // category as port 38028's avoidance below, but with a confirmed root
    // cause this time) — avoid it.
    start_master(38040);
    TCPConnection client = connect_client(38040);
    register_three_nodes(client);

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
    register_three_nodes(client);

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
    register_three_nodes(client);

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
    register_three_nodes(client);

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
    // Port 38028 specifically fails to bind in this sandbox for unknown
    // reasons (same category of environment quirk as port 36004 — see
    // BILJESKE_FAZE.md); avoid it.
    start_master(38032);
    TCPConnection client = connect_client(38032);
    register_three_nodes(client);

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

TEST_F(MasterTest, ReadFileReturnsReplicaListPerBlock) {
    start_master(38020);
    TCPConnection client = connect_client(38020);
    register_three_nodes(client);

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

    // Faza 9 wire format: [u64 id][u64 size][u64 mtime][u32 block_count]
    // { [u64 block_id][u32 replica_count]
    //   {[u32 host_len][host][u16 port]} x replica_count } x block_count
    // (see docs/design.md §13).
    uint32_t block_count = get_u32(resp->payload.data() + 24);
    ASSERT_EQ(block_count, 1u);
    size_t offset = 28;
    BlockId returned_block_id = get_u64(resp->payload.data() + offset);
    offset += sizeof(uint64_t);
    EXPECT_EQ(returned_block_id, block_id);

    uint32_t replica_count = get_u32(resp->payload.data() + offset);
    offset += sizeof(uint32_t);
    ASSERT_EQ(replica_count, 3u);

    std::set<uint16_t> ports;
    for (uint32_t i = 0; i < replica_count; ++i) {
        uint32_t host_length = get_u32(resp->payload.data() + offset);
        offset += sizeof(uint32_t);
        std::string host = get_string(resp->payload.data() + offset, host_length);
        offset += host_length;
        uint16_t port = get_u16(resp->payload.data() + offset);
        offset += sizeof(uint16_t);
        EXPECT_EQ(host, "127.0.0.1");
        ports.insert(port);
    }
    EXPECT_EQ(ports.size(), 3u);
    EXPECT_EQ(resp->payload.size(), offset);
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

// Faza 11: DELETE_FILE / WRITE_COMMIT-overwrite orphan-block GC dispatch
// (see docs/design.md §15).

TEST_F(MasterTest, DeleteFileDispatchesDeleteBlockToAllRegisteredReplicas) {
    start_master(38033);
    TCPConnection client = connect_client(38033);
    auto nodes = register_three_recording_nodes(client, 38133, 38134, 38135);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    BlockId block_id = get_u64(alloc_resp->payload.data());
    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(3, "/a.txt", {block_id}, /*size=*/10, 10)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::DELETE_FILE, 4, "/a.txt")));
    auto delete_resp = client.receiveMessage();
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->type, MessageType::ACK);

    for (auto& node : nodes) {
        EXPECT_TRUE(node->received(block_id));
    }
}

TEST_F(MasterTest, DeleteFileWithUnreachableReplicaStillReturnsAckToClient) {
    // register_three_nodes() points at ports nothing listens on — exercises
    // the best-effort/non-fatal dispatch policy (see docs/design.md §15).
    start_master(38034);
    TCPConnection client = connect_client(38034);
    register_three_nodes(client);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    BlockId block_id = get_u64(alloc_resp->payload.data());
    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(3, "/a.txt", {block_id}, /*size=*/10, 10)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::DELETE_FILE, 4, "/a.txt")));
    auto delete_resp = client.receiveMessage();
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->type, MessageType::ACK);
}

TEST_F(MasterTest, WriteCommitOverwriteDispatchesDeleteBlockForOldBlocksOnly) {
    start_master(38035);
    TCPConnection client = connect_client(38035);
    auto nodes = register_three_recording_nodes(client, 38136, 38137, 38138);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto first_alloc = client.receiveMessage();
    ASSERT_TRUE(first_alloc.has_value());
    BlockId old_block = get_u64(first_alloc->payload.data());
    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(3, "/a.txt", {old_block}, /*size=*/10, 10)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 4, "/a.txt")));
    auto second_alloc = client.receiveMessage();
    ASSERT_TRUE(second_alloc.has_value());
    BlockId new_block = get_u64(second_alloc->payload.data());
    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(5, "/a.txt", {new_block}, /*size=*/5, 5)));
    auto overwrite_resp = client.receiveMessage();
    ASSERT_TRUE(overwrite_resp.has_value());
    EXPECT_EQ(overwrite_resp->type, MessageType::ACK);

    for (auto& node : nodes) {
        EXPECT_TRUE(node->received(old_block));
        EXPECT_FALSE(node->received(new_block));
    }
}

TEST_F(MasterTest, DeleteNonexistentFileReturnsErrorWithoutDispatch) {
    start_master(38036);
    TCPConnection client = connect_client(38036);
    auto nodes = register_three_recording_nodes(client, 38139, 38140, 38141);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::DELETE_FILE, 1, "/nope.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::FILE_NOT_FOUND);

    for (auto& node : nodes) {
        std::lock_guard<std::mutex> lock(node->mutex);
        EXPECT_TRUE(node->deleted_block_ids.empty());
    }
}

TEST_F(MasterTest, DeleteFileWithHangingReplicaCompletesWithinBoundedTime) {
    // Regression test for the CRITICAL fix to send_delete_block()'s
    // previously-unbounded blocking recv(): a node that accepts the
    // connection but never replies within DELETE_BLOCK_TIMEOUT_MS must not
    // hang the worker thread handling the client's DELETE_FILE forever (see
    // docs/design.md §15).
    start_master(38037);
    TCPConnection client = connect_client(38037);

    auto hanging_node = HangingNode::start(38142, DELETE_BLOCK_TIMEOUT_MS + 500);
    auto fast_node_a = RecordingNode::start(38143);
    auto fast_node_b = RecordingNode::start(38144);

    ASSERT_TRUE(client.sendMessage(make_register_node_request(
        850, /*node_id=*/1, "127.0.0.1", 38142, /*capacity_bytes=*/1024)));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_register_node_request(
        851, /*node_id=*/2, "127.0.0.1", 38143, /*capacity_bytes=*/1024)));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_register_node_request(
        852, /*node_id=*/3, "127.0.0.1", 38144, /*capacity_bytes=*/1024)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    BlockId block_id = get_u64(alloc_resp->payload.data());
    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(3, "/a.txt", {block_id}, /*size=*/10, 10)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::DELETE_FILE, 4, "/a.txt")));
    auto delete_resp = client.receiveMessage();
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->type, MessageType::ACK);
    // Bounded by ~DELETE_BLOCK_TIMEOUT_MS for the one hanging replica, plus
    // slack for the two fast replicas — nowhere near "forever".
    EXPECT_LT(elapsed, std::chrono::milliseconds(DELETE_BLOCK_TIMEOUT_MS * 2));
}

// Faza 12.1: HEARTBEAT (see docs/design.md §16).

TEST_F(MasterTest, RegisterThenHeartbeatReturnsAck) {
    start_master(38038);
    TCPConnection client = connect_client(38038);

    ASSERT_TRUE(client.sendMessage(
        make_register_node_request(1, /*node_id=*/1, "127.0.0.1", 9101, /*capacity_bytes=*/1024)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_heartbeat_request(2, /*node_id=*/1)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
    ASSERT_EQ(resp->payload.size(), 1u);
    EXPECT_EQ(static_cast<uint8_t>(resp->payload[0]), 0u);  // needs_inventory always 0 in 12.1
}

TEST_F(MasterTest, HeartbeatFromUnknownNodeReturnsNodeNotFound) {
    start_master(38039);
    TCPConnection client = connect_client(38039);

    ASSERT_TRUE(client.sendMessage(make_heartbeat_request(1, /*node_id=*/99)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::NODE_NOT_FOUND);
}

// Faza 12.3: NODE_INVENTORY (see docs/design.md §16).

TEST_F(MasterTest, NodeInventoryLogsUnknownBlockAsOrphanWithoutDeleting) {
    start_master(38150);
    TCPConnection client = connect_client(38150);
    auto nodes = register_three_recording_nodes(client, 38151, 38152, 38153);

    // node 1 reports a block Master has never heard of — §8.34 "true
    // orphan": logged only, never dispatched for physical cleanup (see
    // docs/design.md §16).
    ASSERT_TRUE(client.sendMessage(
        make_node_inventory_request(1, /*node_id=*/1, {InventoryEntry{99999, 1, 0}})));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    for (auto& node : nodes) {
        std::lock_guard<std::mutex> lock(node->mutex);
        EXPECT_TRUE(node->deleted_block_ids.empty());
        EXPECT_TRUE(node->copy_block_ids.empty());
    }
}

TEST_F(MasterTest, NodeInventoryMarksStaleReplicaForResync) {
    start_master(38154);
    TCPConnection client = connect_client(38154);
    auto nodes = register_three_recording_nodes(client, 38155, 38156, 38157);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    BlockId block_id = get_u64(alloc_resp->payload.data());
    // Freshly allocated — version 1 (see set_block_replicas()).

    // Node 1 (one of the 3 replicas) reports a stale version (0 < 1) — must
    // trigger a resync: Master dispatches COPY_BLOCK to some OTHER healthy
    // replica, naming node 1 as the destination (see docs/design.md §16,
    // RecoveryManager::handle_stale_replica()).
    ASSERT_TRUE(client.sendMessage(
        make_node_inventory_request(3, /*node_id=*/1, {InventoryEntry{block_id, 0, 0}})));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    bool some_other_node_got_copy_block =
        nodes[1]->received_copy_block(block_id) || nodes[2]->received_copy_block(block_id);
    EXPECT_TRUE(some_other_node_got_copy_block);
    EXPECT_FALSE(nodes[0]->received_copy_block(block_id));  // never itself as the source
}

TEST_F(MasterTest, NodeInventoryVersionAheadOfMasterIsLoggedWithoutAction) {
    // Bugfix regression: reported_version > known_version used to be
    // silently ignored entirely (no branch handled it at all). Now it's
    // logged as an anomaly — this test confirms the ACK still comes back
    // and, crucially, that nothing destructive or incorrect happens: no
    // COPY_BLOCK (that would be resync logic for the OPPOSITE, stale
    // case), no DELETE_BLOCK (that's the == case's over-replication
    // cleanup) — see docs/design.md §16.
    start_master(38161);
    TCPConnection client = connect_client(38161);
    auto nodes = register_three_recording_nodes(client, 38162, 38163, 38164);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    BlockId block_id = get_u64(alloc_resp->payload.data());
    // Freshly allocated — Master's known version is 1.

    ASSERT_TRUE(client.sendMessage(
        make_node_inventory_request(3, /*node_id=*/1, {InventoryEntry{block_id, /*version=*/2, 0}})));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);

    for (auto& node : nodes) {
        std::lock_guard<std::mutex> lock(node->mutex);
        EXPECT_TRUE(node->deleted_block_ids.empty());
        EXPECT_TRUE(node->copy_block_ids.empty());
    }
}

// Faza 12.4: Master persistence + restart (see docs/design.md §16).

TEST_F(MasterTest, MasterRestartPreservesNamespaceAndBlockMetadata) {
    constexpr uint16_t port = 38158;
    std::filesystem::path metadata_dir = dir_ / "master_metadata";
    BlockId block_id = 0;

    {
        Master::Config config{port, metadata_dir};
        auto result = Master::create(config);
        ASSERT_TRUE(result.is_ok());
        master_ = std::move(result.value());
        ASSERT_EQ(master_->start(), ErrorCode::OK);

        TCPConnection client = connect_client(port);
        register_three_nodes(client);

        ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
        ASSERT_TRUE(client.receiveMessage().has_value());
        ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
        auto alloc_resp = client.receiveMessage();
        ASSERT_TRUE(alloc_resp.has_value());
        block_id = get_u64(alloc_resp->payload.data());

        ASSERT_TRUE(
            client.sendMessage(make_write_commit_request(3, "/a.txt", {block_id}, /*size=*/42, 42)));
        ASSERT_TRUE(client.receiveMessage().has_value());

        master_->stop();
        master_.reset();  // destroyed here, simulating a process restart
    }

    // Fresh Master instance over the SAME metadata_dir — no storage nodes
    // re-registered yet, purely testing namespace/block metadata recovery.
    Master::Config restarted_config{port, metadata_dir};
    auto restarted_result = Master::create(restarted_config);
    ASSERT_TRUE(restarted_result.is_ok());
    master_ = std::move(restarted_result.value());
    ASSERT_EQ(master_->start(), ErrorCode::OK);

    TCPConnection client = connect_client(port);
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::STAT_FILE, 4, "/a.txt")));
    auto stat_resp = client.receiveMessage();
    ASSERT_TRUE(stat_resp.has_value());
    EXPECT_EQ(stat_resp->type, MessageType::ACK);

    uint64_t size = get_u64(stat_resp->payload.data() + 8);
    uint32_t block_count = get_u32(stat_resp->payload.data() + 24);
    BlockId restored_block_id = get_u64(stat_resp->payload.data() + 28);
    EXPECT_EQ(size, 42u);
    EXPECT_EQ(block_count, 1u);
    EXPECT_EQ(restored_block_id, block_id);
}

TEST_F(MasterTest, MasterRestartRestoresNodesAsJoiningNotHealthy) {
    constexpr uint16_t port = 38159;
    std::filesystem::path metadata_dir = dir_ / "master_metadata";

    {
        Master::Config config{port, metadata_dir};
        auto result = Master::create(config);
        ASSERT_TRUE(result.is_ok());
        master_ = std::move(result.value());
        ASSERT_EQ(master_->start(), ErrorCode::OK);

        TCPConnection client = connect_client(port);
        register_three_nodes(client);
        ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/x.txt")));
        ASSERT_TRUE(client.receiveMessage().has_value());

        master_->stop();
        master_.reset();
    }

    Master::Config restarted_config{port, metadata_dir};
    auto restarted_result = Master::create(restarted_config);
    ASSERT_TRUE(restarted_result.is_ok());
    master_ = std::move(restarted_result.value());
    ASSERT_EQ(master_->start(), ErrorCode::OK);

    // The 3 previously-registered nodes are known to Master but restored
    // as JOINING, not HEALTHY — no live proof they're still up after a
    // restart (see docs/design.md §16). A fresh ALLOCATE_BLOCK must fail
    // exactly like the zero-registered-nodes case, until nodes re-register
    // or send inventory.
    TCPConnection client = connect_client(port);
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 1, "/x.txt")));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::NODE_NOT_FOUND);
}

TEST_F(MasterTest, HeartbeatFromJoiningNodeSetsNeedsInventoryBit) {
    constexpr uint16_t port = 38160;
    std::filesystem::path metadata_dir = dir_ / "master_metadata";

    {
        Master::Config config{port, metadata_dir};
        auto result = Master::create(config);
        ASSERT_TRUE(result.is_ok());
        master_ = std::move(result.value());
        ASSERT_EQ(master_->start(), ErrorCode::OK);

        TCPConnection client = connect_client(port);
        register_three_nodes(client);

        master_->stop();
        master_.reset();
    }

    Master::Config restarted_config{port, metadata_dir};
    auto restarted_result = Master::create(restarted_config);
    ASSERT_TRUE(restarted_result.is_ok());
    master_ = std::move(restarted_result.value());
    ASSERT_EQ(master_->start(), ErrorCode::OK);

    TCPConnection client = connect_client(port);
    ASSERT_TRUE(client.sendMessage(make_heartbeat_request(1, /*node_id=*/1)));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
    ASSERT_EQ(resp->payload.size(), 1u);
    EXPECT_EQ(static_cast<uint8_t>(resp->payload[0]), 1u);  // needs_inventory — node is JOINING
}

// Faza 14: see docs/design.md §18.
TEST_F(MasterTest, GetStatsReturnsTextSnapshotReflectingPriorOperations) {
    constexpr uint16_t port = 38200;
    start_master(port);
    TCPConnection client = connect_client(port);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::STAT_FILE, 2, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(Message{MessageType::GET_STATS, 3, {}}));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::STATS_RESPONSE);

    std::string text(reinterpret_cast<const char*>(resp->payload.data()), resp->payload.size());
    EXPECT_NE(text.find("DFS Statistics"), std::string::npos);
    EXPECT_NE(text.find("CREATE: total=1"), std::string::npos);
    EXPECT_NE(text.find("STAT: total=1"), std::string::npos);

    StatisticsSnapshot snap = master_->statistics_snapshot();
    EXPECT_EQ(snap.operations[static_cast<size_t>(OperationType::CREATE)].successful, 1u);
    EXPECT_EQ(snap.operations[static_cast<size_t>(OperationType::STAT)].successful, 1u);
}

TEST_F(MasterTest, WriteCommitRecordsReplicationStats) {
    constexpr uint16_t port = 38201;
    start_master(port);
    TCPConnection client = connect_client(port);
    register_three_nodes(client);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    ASSERT_EQ(alloc_resp->type, MessageType::ACK);
    BlockId block_id = get_u64(alloc_resp->payload.data());

    constexpr uint64_t kSize = 42;
    ASSERT_TRUE(client.sendMessage(
        make_write_commit_request(3, "/a.txt", {block_id}, kSize)));
    auto commit_resp = client.receiveMessage();
    ASSERT_TRUE(commit_resp.has_value());
    ASSERT_EQ(commit_resp->type, MessageType::ACK);

    StatisticsSnapshot snap = master_->statistics_snapshot();
    EXPECT_EQ(snap.replication_operations, 1u);
    EXPECT_EQ(snap.bytes_replicated, kSize);
    EXPECT_EQ(snap.operations[static_cast<size_t>(OperationType::WRITE)].successful, 1u);
    // Regression test: this used to be hardcoded to 0 regardless
    // of the real committed size (see docs/design.md §18).
    EXPECT_EQ(snap.operations[static_cast<size_t>(OperationType::WRITE)].bytes, kSize);
}

// Faza 18: ABANDON_BLOCKS — Client-triggered cleanup after a failed write
// attempt (see docs/design_faza18_abandon_blocks.md).

TEST_F(MasterTest, AbandonBlocksDispatchesDeleteBlockToAllReplicas) {
    start_master(38300);
    TCPConnection client = connect_client(38300);
    auto nodes = register_three_recording_nodes(client, 38301, 38302, 38303);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    ASSERT_EQ(alloc_resp->type, MessageType::ACK);
    BlockId block_id = get_u64(alloc_resp->payload.data());

    ASSERT_TRUE(client.sendMessage(make_abandon_blocks_request(3, "/a.txt", {block_id})));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
    for (auto& node : nodes) {
        EXPECT_TRUE(node->received(block_id));
    }

    // The abandoned id can no longer be committed.
    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(4, "/a.txt", {block_id}, /*size=*/10, 10)));
    auto commit_resp = client.receiveMessage();
    ASSERT_TRUE(commit_resp.has_value());
    EXPECT_EQ(commit_resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(commit_resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, AbandonBlocksDoesNotTouchCommittedBlocks) {
    start_master(38304);
    TCPConnection client = connect_client(38304);
    auto nodes = register_three_recording_nodes(client, 38305, 38306, 38307);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    BlockId block_id = get_u64(alloc_resp->payload.data());
    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(3, "/a.txt", {block_id}, /*size=*/10, 10)));
    ASSERT_TRUE(client.receiveMessage().has_value());

    ASSERT_TRUE(client.sendMessage(make_abandon_blocks_request(4, "/a.txt", {block_id})));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
    for (auto& node : nodes) {
        EXPECT_FALSE(node->received(block_id));
    }

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::STAT_FILE, 5, "/a.txt")));
    auto stat_resp = client.receiveMessage();
    ASSERT_TRUE(stat_resp.has_value());
    ASSERT_EQ(stat_resp->type, MessageType::ACK);
    EXPECT_EQ(get_u32(stat_resp->payload.data() + 24), 1u);  // block_count
}

TEST_F(MasterTest, AbandonBlocksOnUnknownPathReturnsAckWithoutDispatch) {
    start_master(38308);
    TCPConnection client = connect_client(38308);
    auto nodes = register_three_recording_nodes(client, 38309, 38310, 38311);

    ASSERT_TRUE(client.sendMessage(make_abandon_blocks_request(1, "/nope.txt", {1, 2})));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
    for (auto& node : nodes) {
        EXPECT_FALSE(node->received(1));
        EXPECT_FALSE(node->received(2));
    }
}

TEST_F(MasterTest, AbandonBlocksWithUnreachableReplicaStillReturnsAck) {
    // register_three_nodes() points at ports nothing listens on — the
    // cleanup is best-effort, same policy as DELETE_FILE (see
    // docs/design.md §15).
    start_master(38312);
    TCPConnection client = connect_client(38312);
    register_three_nodes(client);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    BlockId block_id = get_u64(alloc_resp->payload.data());

    ASSERT_TRUE(client.sendMessage(make_abandon_blocks_request(3, "/a.txt", {block_id})));
    auto resp = client.receiveMessage();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->type, MessageType::ACK);
}

TEST_F(MasterTest, AbandonBlocksRejectsMalformedPayload) {
    start_master(38313);
    TCPConnection client = connect_client(38313);

    auto expect_invalid = [&](std::vector<std::byte> payload, uint64_t request_id) {
        ASSERT_TRUE(
            client.sendMessage(Message{MessageType::ABANDON_BLOCKS, request_id, std::move(payload)}));
        auto resp = client.receiveMessage();
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->type, MessageType::ERROR);
        EXPECT_EQ(error_from_payload(resp->payload), ErrorCode::INVALID_REQUEST);
    };

    // Empty payload.
    expect_invalid({}, 1);

    // Path length claims more bytes than present.
    {
        std::vector<std::byte> payload;
        put_u32(payload, 100);
        expect_invalid(std::move(payload), 2);
    }

    // Missing block_count.
    {
        Message full = make_abandon_blocks_request(0, "/a.txt", {});
        full.payload.resize(full.payload.size() - sizeof(uint32_t));
        expect_invalid(std::move(full.payload), 3);
    }

    // block_count says 2, only one id present.
    {
        Message full = make_abandon_blocks_request(0, "/a.txt", {7, 8});
        full.payload.resize(full.payload.size() - sizeof(uint64_t));
        expect_invalid(std::move(full.payload), 4);
    }

    // Trailing garbage after the declared ids.
    {
        Message full = make_abandon_blocks_request(0, "/a.txt", {7});
        full.payload.push_back(std::byte{0});
        expect_invalid(std::move(full.payload), 5);
    }
}

TEST_F(MasterTest, AllocateBlockWithNoLiveNodesLeavesNothingPending) {
    // Without any registered node, ALLOCATE_BLOCK fails with NODE_NOT_FOUND
    // after NamespaceManager already issued block id 1 (next_block_id_
    // starts at 1). The Client never learns that id, so Master itself must
    // drop it — a later commit naming it has to be rejected.
    start_master(38314);
    TCPConnection client = connect_client(38314);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto alloc_resp = client.receiveMessage();
    ASSERT_TRUE(alloc_resp.has_value());
    ASSERT_EQ(alloc_resp->type, MessageType::ERROR);
    ASSERT_EQ(error_from_payload(alloc_resp->payload), ErrorCode::NODE_NOT_FOUND);

    ASSERT_TRUE(client.sendMessage(
        make_write_commit_request(3, "/a.txt", {/*block_id=*/1}, /*size=*/10, 10)));
    auto commit_resp = client.receiveMessage();
    ASSERT_TRUE(commit_resp.has_value());
    EXPECT_EQ(commit_resp->type, MessageType::ERROR);
    EXPECT_EQ(error_from_payload(commit_resp->payload), ErrorCode::INVALID_REQUEST);
}

TEST_F(MasterTest, WriteCommitDispatchesDeleteForLeftoverPendingBlocks) {
    start_master(38315);
    TCPConnection client = connect_client(38315);
    auto nodes = register_three_recording_nodes(client, 38316, 38317, 38318);

    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::CREATE_FILE, 1, "/a.txt")));
    ASSERT_TRUE(client.receiveMessage().has_value());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 2, "/a.txt")));
    auto leftover_alloc = client.receiveMessage();
    ASSERT_TRUE(leftover_alloc.has_value());
    BlockId leftover = get_u64(leftover_alloc->payload.data());
    ASSERT_TRUE(client.sendMessage(make_path_request(MessageType::ALLOCATE_BLOCK, 3, "/a.txt")));
    auto committed_alloc = client.receiveMessage();
    ASSERT_TRUE(committed_alloc.has_value());
    BlockId committed = get_u64(committed_alloc->payload.data());

    ASSERT_TRUE(
        client.sendMessage(make_write_commit_request(4, "/a.txt", {committed}, /*size=*/10, 10)));
    auto commit_resp = client.receiveMessage();
    ASSERT_TRUE(commit_resp.has_value());
    ASSERT_EQ(commit_resp->type, MessageType::ACK);

    for (auto& node : nodes) {
        EXPECT_TRUE(node->received(leftover));
        EXPECT_FALSE(node->received(committed));
    }
}
