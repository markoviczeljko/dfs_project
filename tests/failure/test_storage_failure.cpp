#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "process_cluster.h"

// Faza 16 (§16.4): real failure-injection test. Bring up Master + 3
// Storage (RF=3) plus a 4th, initially-idle spare node — a real WRITE
// replicates onto exactly 3 of the 4, guaranteeing a genuinely fresh
// recovery destination exists once one of the 3 is killed (see
// docs/design.md §20 for why an exact-3-node cluster can't exercise
// recovery at all: ReplicationManager::choose_recovery_destination()
// returns nullopt once every alive node is already a replica holder,
// which is unconditionally true with only 3 total nodes and RF=3).
// SIGKILL (hard, real OS-level termination) — not a graceful stop — kills
// one real Storage Node process mid-cluster-operation; Master's real
// FailureDetector must notice, RecoveryManager must re-replicate onto the
// spare, and every dfs_client READ issued throughout must keep
// succeeding.
class StorageFailureTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_failure_storage_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(StorageFailureTest, KilledNodeTriggersRecoveryAndReadKeepsSucceeding) {
    using namespace dfs_test;

    constexpr uint16_t kMasterPort = 41710;
    constexpr uint16_t kStorage1Port = 41711;
    constexpr uint16_t kStorage2Port = 41712;
    constexpr uint16_t kStorage3Port = 41713;
    constexpr uint16_t kStorage4Port = 41714;  // spare

    auto cluster_opt = spawn_cluster(
        dir_, kMasterPort, {kStorage1Port, kStorage2Port, kStorage3Port, kStorage4Port},
        std::chrono::seconds(5));
    ASSERT_TRUE(cluster_opt.has_value()) << "cluster failed to come up";
    SpawnedCluster cluster = std::move(*cluster_opt);
    ASSERT_EQ(cluster.storage_nodes.size(), 4u);

    std::filesystem::path src = dir_ / "src.txt";
    {
        std::ofstream out(src);
        out << std::string(500, 'x');
    }

    ASSERT_EQ(run_client(kMasterPort, {"create", "/failure.txt"}), 0);
    ASSERT_EQ(run_client(kMasterPort, {"write", "/failure.txt", src.string()}), 0);

    // Find which of the 4 nodes actually hold the (single) block, and
    // kill one of them — the uninvolved 4th node is guaranteed to be a
    // fresh, valid recovery destination.
    std::vector<size_t> holders;
    for (size_t i = 0; i < cluster.storage_data_dirs.size(); ++i) {
        if (has_any_block_on_disk(cluster.storage_data_dirs[i])) {
            holders.push_back(i);
        }
    }
    ASSERT_EQ(holders.size(), 3u);
    size_t killed = holders[0];

    ASSERT_EQ(cluster.storage_nodes[killed].send_signal(SIGKILL), ErrorCode::OK);
    cluster.storage_nodes[killed].wait_for_exit(std::chrono::seconds(2));

    // READ must keep succeeding throughout — poll while waiting for real
    // failure detection + recovery in the background (real
    // SUSPECTED_TIMEOUT_SECONDS + DEAD_TIMEOUT_SECONDS, so this genuinely
    // takes double-digit seconds; not shortened, see docs/design.md §20).
    std::filesystem::path dst = dir_ / "dst.txt";
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    bool recovered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        ASSERT_EQ(run_client(kMasterPort, {"read", "/failure.txt", dst.string()}), 0)
            << "READ must keep succeeding during recovery";

        int replicas_on_disk = 0;
        for (size_t i = 0; i < cluster.storage_data_dirs.size(); ++i) {
            if (i != killed && has_any_block_on_disk(cluster.storage_data_dirs[i])) {
                ++replicas_on_disk;
            }
        }
        if (replicas_on_disk == 3) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_TRUE(recovered) << "recovery did not complete within the deadline";

    // Final read: content matches exactly what was written.
    ASSERT_EQ(run_client(kMasterPort, {"read", "/failure.txt", dst.string()}), 0);
    std::ifstream expected(src);
    std::ifstream actual(dst);
    std::string expected_content((std::istreambuf_iterator<char>(expected)),
                                  std::istreambuf_iterator<char>());
    std::string actual_content((std::istreambuf_iterator<char>(actual)), std::istreambuf_iterator<char>());
    EXPECT_EQ(actual_content, expected_content);

    cluster.stop_all();
}
