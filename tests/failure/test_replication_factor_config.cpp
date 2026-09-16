#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "process_cluster.h"

// Faza 17.1 (§16.6-§16.8 prep, docs/design.md §21): confirms
// replication_factor is a real runtime Master config knob end to end, not
// just a ReplicationManager constructor parameter that compiles but is
// never actually wired up — Faza 17.4's RF=1-vs-RF=3 benchmark depends on
// this genuinely working, not just building.
class ReplicationFactorConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_rf_config_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(ReplicationFactorConfigTest, ConfiguredReplicationFactorOfOnePlacesExactlyOneReplica) {
    using namespace dfs_test;

    constexpr uint16_t kMasterPort = 41730;
    constexpr uint16_t kStorage1Port = 41731;
    constexpr uint16_t kStorage2Port = 41732;
    constexpr uint16_t kStorage3Port = 41733;

    auto cluster_opt =
        spawn_cluster(dir_, kMasterPort, {kStorage1Port, kStorage2Port, kStorage3Port},
                      std::chrono::seconds(5), /*replication_factor=*/1);
    ASSERT_TRUE(cluster_opt.has_value()) << "cluster failed to come up";
    SpawnedCluster cluster = std::move(*cluster_opt);
    ASSERT_EQ(cluster.storage_nodes.size(), 3u);

    std::filesystem::path src = dir_ / "src.txt";
    {
        std::ofstream out(src);
        out << std::string(500, 'x');
    }

    ASSERT_EQ(run_client(kMasterPort, {"create", "/rf1.txt"}), 0);
    ASSERT_EQ(run_client(kMasterPort, {"write", "/rf1.txt", src.string()}), 0);

    int holders = 0;
    for (const auto& data_dir : cluster.storage_data_dirs) {
        if (has_any_block_on_disk(data_dir)) ++holders;
    }
    EXPECT_EQ(holders, 1) << "RF=1 must place the block on exactly one node";

    cluster.stop_all();
}

TEST_F(ReplicationFactorConfigTest, DefaultReplicationFactorStillPlacesThreeReplicas) {
    using namespace dfs_test;

    constexpr uint16_t kMasterPort = 41734;
    constexpr uint16_t kStorage1Port = 41735;
    constexpr uint16_t kStorage2Port = 41736;
    constexpr uint16_t kStorage3Port = 41737;

    // No replication_factor argument — regression guard that the default
    // parameter still reproduces the pre-Faza-17.1 RF=3 behavior.
    auto cluster_opt = spawn_cluster(dir_, kMasterPort, {kStorage1Port, kStorage2Port, kStorage3Port},
                                      std::chrono::seconds(5));
    ASSERT_TRUE(cluster_opt.has_value()) << "cluster failed to come up";
    SpawnedCluster cluster = std::move(*cluster_opt);

    std::filesystem::path src = dir_ / "src.txt";
    {
        std::ofstream out(src);
        out << std::string(500, 'x');
    }

    ASSERT_EQ(run_client(kMasterPort, {"create", "/rf3.txt"}), 0);
    ASSERT_EQ(run_client(kMasterPort, {"write", "/rf3.txt", src.string()}), 0);

    int holders = 0;
    for (const auto& data_dir : cluster.storage_data_dirs) {
        if (has_any_block_on_disk(data_dir)) ++holders;
    }
    EXPECT_EQ(holders, 3) << "default replication_factor must still be 3";

    cluster.stop_all();
}
