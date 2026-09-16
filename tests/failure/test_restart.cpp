#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "process_cluster.h"

// Faza 16 (§16.5): real multi-process restart test. WRITE -> gracefully
// stop one real Storage Node process (SIGTERM) -> restart the SAME
// process against the SAME data_dir -> READ must still return the
// original bytes, proving real restart-from-disk local recovery (not an
// in-memory object that never actually lost its state, unlike
// tests/integration/test_recovery.cpp's equivalent in-process case).
class RestartFailureTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_failure_restart_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(RestartFailureTest, StopThenRestartStorageNodePreservesData) {
    using namespace dfs_test;

    constexpr uint16_t kMasterPort = 41700;
    constexpr uint16_t kStorage1Port = 41701;
    constexpr uint16_t kStorage2Port = 41702;
    constexpr uint16_t kStorage3Port = 41703;

    auto cluster_opt = spawn_cluster(dir_, kMasterPort, {kStorage1Port, kStorage2Port, kStorage3Port},
                                      std::chrono::seconds(5));
    ASSERT_TRUE(cluster_opt.has_value()) << "cluster failed to come up";
    SpawnedCluster cluster = std::move(*cluster_opt);

    std::filesystem::path src = dir_ / "src.txt";
    {
        std::ofstream out(src);
        out << "hello from a real multi-process restart test";
    }
    std::filesystem::path dst = dir_ / "dst.txt";

    ASSERT_EQ(run_client(kMasterPort, {"create", "/restart.txt"}), 0);
    ASSERT_EQ(run_client(kMasterPort, {"write", "/restart.txt", src.string()}), 0);

    // Graceful stop of Storage Node 1 (SIGTERM, not SIGKILL — this test is
    // about a clean restart cycle, not failure injection).
    ASSERT_EQ(cluster.storage_nodes[0].send_signal(SIGTERM), ErrorCode::OK);
    auto exit_status = cluster.storage_nodes[0].wait_for_exit(std::chrono::seconds(5));
    ASSERT_TRUE(exit_status.has_value()) << "storage node 1 did not exit after SIGTERM";

    // Restart the SAME process — same config file, so the same node_id/
    // port/data_dir (real local recovery reads whatever's already on disk
    // under that data_dir).
    auto restarted = respawn_storage_node(cluster.storage_configs[0]);
    ASSERT_TRUE(restarted.is_ok());
    cluster.storage_nodes[0] = std::move(restarted.value());
    ASSERT_TRUE(wait_for_port_ready("127.0.0.1", kStorage1Port, std::chrono::seconds(5)));

    ASSERT_EQ(run_client(kMasterPort, {"read", "/restart.txt", dst.string()}), 0);

    std::ifstream expected(src);
    std::ifstream actual(dst);
    std::string expected_content((std::istreambuf_iterator<char>(expected)),
                                  std::istreambuf_iterator<char>());
    std::string actual_content((std::istreambuf_iterator<char>(actual)), std::istreambuf_iterator<char>());
    EXPECT_EQ(actual_content, expected_content);

    cluster.stop_all();
}
