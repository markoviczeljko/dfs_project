#include <gtest/gtest.h>

#include <endian.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "client/client.h"
#include "common/config.h"
#include "master/master.h"
#include "network/connection.h"
#include "network/network_server.h"
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

    // Faza 18: an extra (4th+) Storage Node on an already-running cluster —
    // node ids continue after the first three.
    void start_extra_storage_node(uint16_t master_port, uint16_t storage_port) {
        StorageNodeId id = static_cast<StorageNodeId>(storage_nodes_.size() + 1);
        StorageNode::Config storage_config{id,
                                            storage_port,
                                            dir_ / ("node" + std::to_string(id)),
                                            1024ULL * 1024 * 1024,
                                            "127.0.0.1",
                                            "127.0.0.1",
                                            master_port};
        auto storage_result = StorageNode::create(storage_config);
        ASSERT_TRUE(storage_result.is_ok());
        storage_nodes_.push_back(std::move(storage_result.value()));
        ASSERT_EQ(storage_nodes_.back()->start(), ErrorCode::OK);
    }

    // Counts physical block files a node holds (node_index is 0-based) —
    // same on-disk layout test_delete.cpp's block_exists_on_disk() relies on
    // (see BlockStorage::block_path, docs/design.md §15).
    size_t block_files_on_disk(size_t node_index) const {
        std::filesystem::path blocks_dir =
            dir_ / ("node" + std::to_string(node_index + 1)) / "blocks";
        if (!std::filesystem::exists(blocks_dir)) return 0;
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(blocks_dir)) {
            if (entry.path().extension() == ".block") ++count;
        }
        return count;
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

    // Faza 14 (see docs/design.md §18).
    StatisticsSnapshot snap = client_->statistics_snapshot();
    const OperationStats& write_stats = snap.operations[static_cast<size_t>(OperationType::WRITE)];
    EXPECT_EQ(write_stats.total, 1u);
    EXPECT_EQ(write_stats.successful, 1u);
    EXPECT_EQ(write_stats.bytes, data.size());
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

// Faza 18: a failed WRITE must not leave its already-written replicas
// behind — the Client sends ABANDON_BLOCKS and Master GCs them (see
// docs/design_faza18_abandon_blocks.md). A stopped node stays "alive" in
// Master's eyes until FailureDetector's 6s/12s thresholds pass, so it is
// still chosen as a replica and the write to it fails.

TEST_F(WriteIntegrationTest, FailedWriteLeavesNoBlockFilesOnDisk) {
    start_cluster(40700, 40701, 40702, 40703);
    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);

    storage_nodes_[2]->stop();
    std::vector<uint8_t> data(1000, 7);
    EXPECT_NE(client_->write("/a.txt", data), ErrorCode::OK);

    EXPECT_EQ(block_files_on_disk(0), 0u);
    EXPECT_EQ(block_files_on_disk(1), 0u);
}

TEST_F(WriteIntegrationTest, FailedWriteKeepsPreviousFileContent) {
    start_cluster(40710, 40711, 40712, 40713);
    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> first(500, 1);
    ASSERT_EQ(client_->write("/a.txt", first), ErrorCode::OK);

    storage_nodes_[2]->stop();
    std::vector<uint8_t> second(800, 2);
    EXPECT_NE(client_->write("/a.txt", second), ErrorCode::OK);

    auto read_result = client_->read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), first);
    // Only the committed first version's block remains on the live nodes.
    EXPECT_EQ(block_files_on_disk(0), 1u);
    EXPECT_EQ(block_files_on_disk(1), 1u);
}

TEST_F(WriteIntegrationTest, FailedMultiChunkWriteLeavesNoBlockFilesOnDisk) {
    // 4 nodes, RF=3, deterministic round-robin (replication_manager.cpp):
    // chunk 0 -> {1,2,3} (all alive, succeeds), chunk 1 -> {4,1,2} (node 4
    // stopped, fails). Both chunks' blocks must be cleaned up.
    constexpr uint64_t kBlockSize = 1024;
    start_cluster(40720, 40721, 40722, 40723, kBlockSize);
    start_extra_storage_node(40720, 40724);
    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);

    storage_nodes_[3]->stop();
    std::vector<uint8_t> data(2 * kBlockSize - 10, 9);
    EXPECT_NE(client_->write("/a.txt", data), ErrorCode::OK);

    for (size_t node = 0; node < 3; ++node) {
        EXPECT_EQ(block_files_on_disk(node), 0u) << "node " << node + 1;
    }
}

namespace {

void put_u16_be(std::vector<std::byte>& buf, uint16_t value) {
    uint16_t be = htobe16(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}

void put_u32_be(std::vector<std::byte>& buf, uint32_t value) {
    uint32_t be = htobe32(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}

// Fake Storage Nodes that hold every WRITE_BLOCK for delay_ms before
// ACKing, recording how many WRITE_BLOCKs (across all nodes sharing the
// counters) were being served at the same moment.
struct InFlightCounter {
    std::atomic<int> current{0};
    std::atomic<int> max_seen{0};
};

struct SlowWriteNode {
    std::unique_ptr<NetworkServer> server;

    static std::unique_ptr<SlowWriteNode> start(uint16_t port, int delay_ms,
                                                InFlightCounter& counter) {
        auto node = std::make_unique<SlowWriteNode>();
        auto result = NetworkServer::create(port, [delay_ms, &counter](const Message& request) -> Message {
            if (request.type == MessageType::WRITE_BLOCK) {
                int now = ++counter.current;
                int seen = counter.max_seen.load();
                while (now > seen && !counter.max_seen.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                --counter.current;
            }
            return Message{MessageType::ACK, request.request_id, {}};
        });
        EXPECT_TRUE(result.is_ok());
        node->server = std::move(result.value());
        EXPECT_EQ(node->server->start(), ErrorCode::OK);
        return node;
    }
};

}  // namespace

TEST_F(WriteIntegrationTest, WriteSendsBlockToReplicasInParallel) {
    // Three replicas that each hold a WRITE_BLOCK for kDelayMs: with a
    // parallel fan-out all three are in flight at once; a sequential one
    // never has more than one outstanding.
    constexpr int kDelayMs = 300;
    InFlightCounter counter;
    constexpr uint16_t kMasterPort = 40730;
    Master::Config master_config{kMasterPort, dir_ / "master_metadata"};
    auto master_result = Master::create(master_config);
    ASSERT_TRUE(master_result.is_ok());
    master_ = std::move(master_result.value());
    ASSERT_EQ(master_->start(), ErrorCode::OK);

    std::vector<std::unique_ptr<SlowWriteNode>> slow_nodes;
    auto socket_result = Socket::create();
    ASSERT_TRUE(socket_result.is_ok());
    Socket socket = std::move(socket_result.value());
    ASSERT_EQ(socket.connect("127.0.0.1", kMasterPort), ErrorCode::OK);
    TCPConnection master_connection(std::move(socket));
    for (uint16_t i = 1; i <= 3; ++i) {
        uint16_t port = static_cast<uint16_t>(kMasterPort + i);
        slow_nodes.push_back(SlowWriteNode::start(port, kDelayMs, counter));
        // REGISTER_NODE: [u64 node_id][u32 host_len][host][u16 port][u64 capacity].
        std::vector<std::byte> payload;
        put_u64(payload, i);
        const std::string host = "127.0.0.1";
        put_u32_be(payload, static_cast<uint32_t>(host.size()));
        payload.insert(payload.end(), reinterpret_cast<const std::byte*>(host.data()),
                       reinterpret_cast<const std::byte*>(host.data()) + host.size());
        put_u16_be(payload, port);
        put_u64(payload, 1024ULL * 1024 * 1024);
        ASSERT_TRUE(master_connection.sendMessage(
            Message{MessageType::REGISTER_NODE, static_cast<uint64_t>(100 + i), std::move(payload)}));
        auto resp = master_connection.receiveMessage();
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->type, MessageType::ACK);
    }

    auto client_result = Client::create(Client::Config{"127.0.0.1", kMasterPort, BLOCK_SIZE});
    ASSERT_TRUE(client_result.is_ok());
    client_ = std::move(client_result.value());
    ASSERT_EQ(client_->create_file("/a.txt"), ErrorCode::OK);

    ASSERT_EQ(client_->write("/a.txt", std::vector<uint8_t>(100, 3)), ErrorCode::OK);

    EXPECT_EQ(counter.max_seen.load(), 3) << "replicas were not written in parallel";
}
