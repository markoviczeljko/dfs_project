#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "client/client.h"
#include "common/config.h"
#include "master/master.h"
#include "storage/storage_node.h"

// Faza 10 — APPEND (see docs/design.md §14). Same cluster shape as
// WriteIntegrationTest/ReadIntegrationTest: Master + 3 StorageNodes, driven
// by real Client objects.
class AppendIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_append_integration_test_" +
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
    // Returns a fresh Client connected to the cluster; call again for
    // additional independent Client objects (e.g. one per thread in a
    // concurrency test).
    Client& start_cluster(uint16_t master_port, uint16_t storage_port1, uint16_t storage_port2,
                           uint16_t storage_port3, uint64_t block_size = BLOCK_SIZE) {
        Master::Config master_config{master_port};
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

    std::filesystem::path dir_;
    std::vector<std::unique_ptr<StorageNode>> storage_nodes_;
    std::unique_ptr<Master> master_;
    std::vector<std::unique_ptr<Client>> clients_;
    uint16_t master_port_ = 0;
};

TEST_F(AppendIntegrationTest, AppendToEmptyFileIsEquivalentToWrite) {
    // §6.11: appending to a file with no blocks yet is just a normal write.
    Client& client = start_cluster(40300, 40301, 40302, 40303);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    ASSERT_EQ(client.append("/a.txt", data), ErrorCode::OK);

    auto read_result = client.read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}

TEST_F(AppendIntegrationTest, AppendFillsExistingBlockWithoutCrossingBoundary) {
    constexpr uint64_t kSmallBlockSize = 16;
    Client& client = start_cluster(40310, 40311, 40312, 40313, kSmallBlockSize);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> initial(10, 1);  // 10 of 16 bytes used in block 1
    ASSERT_EQ(client.write("/a.txt", initial), ErrorCode::OK);

    std::vector<uint8_t> appended(5, 2);  // fits in the remaining 6 bytes
    ASSERT_EQ(client.append("/a.txt", appended), ErrorCode::OK);

    std::vector<uint8_t> expected = initial;
    expected.insert(expected.end(), appended.begin(), appended.end());

    auto read_result = client.read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), expected);
}

TEST_F(AppendIntegrationTest, AppendCrossingBlockBoundarySplitsCorrectly) {
    // §6.10: append > free space in the last block must fill it, then
    // allocate a new block for the remainder.
    constexpr uint64_t kSmallBlockSize = 16;
    Client& client = start_cluster(40320, 40321, 40322, 40323, kSmallBlockSize);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> initial(10, 1);  // 6 bytes free in block 1
    ASSERT_EQ(client.write("/a.txt", initial), ErrorCode::OK);

    std::vector<uint8_t> appended(20, 2);  // 6 bytes fill block 1, 14 spill into block 2
    ASSERT_EQ(client.append("/a.txt", appended), ErrorCode::OK);

    std::vector<uint8_t> expected = initial;
    expected.insert(expected.end(), appended.begin(), appended.end());

    auto read_result = client.read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), expected);
}

TEST_F(AppendIntegrationTest, MultipleSequentialAppendsAccumulateCorrectly) {
    Client& client = start_cluster(40330, 40331, 40332, 40333);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> expected;
    for (int i = 0; i < 10; ++i) {
        std::vector<uint8_t> chunk(3, static_cast<uint8_t>(i));
        ASSERT_EQ(client.append("/a.txt", chunk), ErrorCode::OK);
        expected.insert(expected.end(), chunk.begin(), chunk.end());
    }

    auto read_result = client.read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), expected);
}

TEST_F(AppendIntegrationTest, ConcurrentAppendsFromTwoClientsDoNotOverlapOrLoseData) {
    // Literal "Definicija gotovog" from IMPLEMENTATION_PLAN.md's Faza 10
    // section: two concurrent client threads, each appending N bytes 100
    // times to the same file — final size must be the exact sum, with no
    // overlap. Two SEPARATE Client objects (not one Client shared across
    // threads) — this tests Master-side serialization, not Client's own
    // thread-safety (see docs/design.md §14).
    Client& client_a = start_cluster(40340, 40341, 40342, 40343);
    Client& client_b = new_client();

    ASSERT_EQ(client_a.create_file("/a.txt"), ErrorCode::OK);

    constexpr uint8_t kMarkerA = 0xAA;
    constexpr uint8_t kMarkerB = 0xBB;
    constexpr size_t kCallSize = 7;
    constexpr int kCallCount = 100;

    auto append_loop = [](Client& client, uint8_t marker, ErrorCode* out_result) {
        std::vector<uint8_t> chunk(kCallSize, marker);
        for (int i = 0; i < kCallCount; ++i) {
            ErrorCode result = client.append("/a.txt", chunk);
            if (result != ErrorCode::OK) {
                *out_result = result;
                return;
            }
        }
        *out_result = ErrorCode::OK;
    };

    ErrorCode result_a = ErrorCode::INTERNAL_ERROR;
    ErrorCode result_b = ErrorCode::INTERNAL_ERROR;
    std::thread thread_a(append_loop, std::ref(client_a), kMarkerA, &result_a);
    std::thread thread_b(append_loop, std::ref(client_b), kMarkerB, &result_b);
    thread_a.join();
    thread_b.join();

    ASSERT_EQ(result_a, ErrorCode::OK);
    ASSERT_EQ(result_b, ErrorCode::OK);

    auto read_result = client_a.read("/a.txt");
    ASSERT_TRUE(read_result.is_ok());
    const std::vector<uint8_t>& content = read_result.value();

    ASSERT_EQ(content.size(), static_cast<size_t>(kCallSize * kCallCount * 2));

    // Every kCallSize-byte run must be entirely one marker — a torn/
    // interleaved write would split a run into fewer than kCallSize
    // identical bytes.
    size_t a_bytes = 0, b_bytes = 0;
    size_t i = 0;
    while (i < content.size()) {
        uint8_t marker = content[i];
        ASSERT_TRUE(marker == kMarkerA || marker == kMarkerB) << "unexpected byte at " << i;
        size_t run_start = i;
        while (i < content.size() && content[i] == marker) ++i;
        size_t run_length = i - run_start;
        EXPECT_EQ(run_length % kCallSize, 0u)
            << "run of " << run_length << " bytes at offset " << run_start
            << " is not a multiple of the per-call size — torn append";
        if (marker == kMarkerA) {
            a_bytes += run_length;
        } else {
            b_bytes += run_length;
        }
    }
    EXPECT_EQ(a_bytes, static_cast<size_t>(kCallSize * kCallCount));
    EXPECT_EQ(b_bytes, static_cast<size_t>(kCallSize * kCallCount));
}
