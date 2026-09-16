#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
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

// Faza 12.2 — recovery / re-replication after a Storage Node is declared
// DEAD (see docs/design.md §16). Master + 4 real StorageNodes: RF=3 means
// any given block lives on 3 of the 4, leaving at least one genuinely
// spare node available as a recovery destination regardless of which
// specific node gets killed.
class RecoveryIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_recovery_integration_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        clients_.clear();
        for (auto& node : storage_nodes_) node->stop();
        if (master_) master_->stop();
        std::filesystem::remove_all(dir_);
    }

    Client& start_cluster(uint16_t master_port, const std::array<uint16_t, 4>& storage_ports) {
        Master::Config master_config{master_port};
        auto master_result = Master::create(master_config);
        EXPECT_TRUE(master_result.is_ok());
        master_ = std::move(master_result.value());
        EXPECT_EQ(master_->start(), ErrorCode::OK);
        master_port_ = master_port;

        for (size_t i = 0; i < storage_ports.size(); ++i) {
            StorageNode::Config config{static_cast<StorageNodeId>(i + 1),
                                        storage_ports[i],
                                        dir_ / ("node" + std::to_string(i + 1)),
                                        1024ULL * 1024 * 1024,
                                        "127.0.0.1",
                                        "127.0.0.1",
                                        master_port};
            auto result = StorageNode::create(config);
            EXPECT_TRUE(result.is_ok());
            storage_nodes_.push_back(std::move(result.value()));
            EXPECT_EQ(storage_nodes_.back()->start(), ErrorCode::OK);
        }

        auto client_result = Client::create(Client::Config{"127.0.0.1", master_port_, BLOCK_SIZE});
        EXPECT_TRUE(client_result.is_ok());
        clients_.push_back(std::move(client_result.value()));
        return *clients_.back();
    }

    bool block_exists_on_disk(size_t node_index, BlockId id) const {
        std::filesystem::path path = dir_ / ("node" + std::to_string(node_index + 1)) / "blocks" /
                                      (std::to_string(id) + ".block");
        return std::filesystem::exists(path);
    }

    std::filesystem::path dir_;
    std::vector<std::unique_ptr<StorageNode>> storage_nodes_;
    std::unique_ptr<Master> master_;
    std::vector<std::unique_ptr<Client>> clients_;
    uint16_t master_port_ = 0;
};

TEST_F(RecoveryIntegrationTest, KilledNodeTriggersRecoveryAndReadKeepsSucceeding) {
    // Literal "definicija gotovog" from IMPLEMENTATION_PLAN.md's Faza 12
    // section: kill a Storage Node mid-cluster-operation, confirm (a)
    // affected blocks recover onto a new replica, (b) READ keeps
    // succeeding throughout, (c) the recovered replica's content matches
    // the original exactly (see docs/design.md §16).
    Client& client = start_cluster(40600, {40601, 40602, 40603, 40604});

    ASSERT_EQ(client.create_file("/r.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(500, 7);
    ASSERT_EQ(client.write("/r.txt", data), ErrorCode::OK);

    auto stat_result = client.stat("/r.txt");
    ASSERT_TRUE(stat_result.is_ok());
    ASSERT_EQ(stat_result.value().blocks.size(), 1u);
    BlockId block_id = stat_result.value().blocks[0];

    // Find which of the 4 nodes actually hold this block, and kill one of
    // them — the 4th (uninvolved) node is guaranteed to be a valid, fresh
    // recovery destination.
    std::vector<size_t> holders;
    for (size_t i = 0; i < 4; ++i) {
        if (block_exists_on_disk(i, block_id)) holders.push_back(i);
    }
    ASSERT_EQ(holders.size(), 3u);
    size_t killed = holders[0];
    storage_nodes_[killed]->stop();

    // READ must keep succeeding throughout — poll it periodically while
    // waiting for failure detection + recovery to complete in the
    // background (SUSPECTED_TIMEOUT_SECONDS + DEAD_TIMEOUT_SECONDS, real
    // config.h values, so this genuinely takes double-digit seconds).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool recovered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto read_result = client.read("/r.txt");
        ASSERT_TRUE(read_result.is_ok()) << "READ must keep succeeding during recovery";
        EXPECT_EQ(read_result.value(), data);

        int replicas_on_disk = 0;
        for (size_t i = 0; i < 4; ++i) {
            if (i != killed && block_exists_on_disk(i, block_id)) ++replicas_on_disk;
        }
        if (replicas_on_disk == 3) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ASSERT_TRUE(recovered) << "recovery did not complete within the deadline";

    // The recovered replica's content matches the original exactly.
    auto final_read = client.read("/r.txt");
    ASSERT_TRUE(final_read.is_ok());
    EXPECT_EQ(final_read.value(), data);
}

TEST_F(RecoveryIntegrationTest, RestartWithValidBlocksSendsInventoryWithoutRecopying) {
    // Faza 12.3: a Storage Node that restarts with its on-disk blocks still
    // intact (a clean stop/restart, not a real crash-and-lose-everything)
    // must report them via NODE_INVENTORY and NOT trigger any re-copying —
    // Master already knows this replica is current, matching version
    // (see docs/design.md §16).
    std::array<uint16_t, 4> storage_ports{40610, 40611, 40612, 40613};
    Client& client = start_cluster(40609, storage_ports);

    ASSERT_EQ(client.create_file("/s.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(500, 9);
    ASSERT_EQ(client.write("/s.txt", data), ErrorCode::OK);

    auto stat_result = client.stat("/s.txt");
    ASSERT_TRUE(stat_result.is_ok());
    ASSERT_EQ(stat_result.value().blocks.size(), 1u);
    BlockId block_id = stat_result.value().blocks[0];

    std::vector<size_t> holders;
    size_t spare = 4;
    for (size_t i = 0; i < 4; ++i) {
        if (block_exists_on_disk(i, block_id)) {
            holders.push_back(i);
        } else {
            spare = i;
        }
    }
    ASSERT_EQ(holders.size(), 3u);
    ASSERT_LT(spare, 4u);
    size_t restarted = holders[0];

    // Stop and immediately restart the same node id against the SAME
    // on-disk directory — its blocks survive, unlike the killed-node
    // scenario above.
    storage_nodes_[restarted]->stop();

    StorageNode::Config config{static_cast<StorageNodeId>(restarted + 1),
                                storage_ports[restarted],
                                dir_ / ("node" + std::to_string(restarted + 1)),
                                1024ULL * 1024 * 1024,
                                "127.0.0.1",
                                "127.0.0.1",
                                master_port_};
    auto result = StorageNode::create(config);
    ASSERT_TRUE(result.is_ok());
    storage_nodes_[restarted] = std::move(result.value());
    ASSERT_EQ(storage_nodes_[restarted]->start(), ErrorCode::OK);

    // start() only returns once register_with_master() AND send_inventory()
    // have both completed — the reconciliation for this test's single
    // reported block already happened synchronously inside Master's
    // handling of that NODE_INVENTORY call by the time start() returns.
    EXPECT_FALSE(block_exists_on_disk(spare, block_id));   // nothing re-copied to the spare
    EXPECT_TRUE(block_exists_on_disk(restarted, block_id));  // the restarted node's own copy, untouched

    auto read_result = client.read("/s.txt");
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), data);
}
