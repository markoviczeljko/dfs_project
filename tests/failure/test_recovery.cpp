#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "process_cluster.h"

namespace {

// Extracts the unsigned integer following `key` within the specific line
// that starts with `line_prefix` (e.g. line_prefix="Recovery:",
// key="operations=" against a snapshot containing both "Replication:
// operations=0 bytes=0" and "Recovery: operations=3 successful=3
// bytes=..." correctly returns 3, not the Replication line's 0 — plain
// text.find("operations=") would match whichever line comes first, and
// "successful=" also appears in every per-OperationType line, so both
// keys need to be scoped to their own line, not searched globally).
// Returns 0 if the line or key isn't found — same "absent means zero"
// convention as every counter this test checks.
uint64_t extract_stat(const std::string& text, const std::string& line_prefix, const std::string& key) {
    size_t line_pos = text.find(line_prefix);
    if (line_pos == std::string::npos) return 0;
    size_t line_end = text.find('\n', line_pos);
    std::string line = text.substr(line_pos, line_end == std::string::npos
                                                  ? std::string::npos
                                                  : line_end - line_pos);
    size_t pos = line.find(key);
    if (pos == std::string::npos) return 0;
    pos += key.size();
    size_t end = pos;
    while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) ++end;
    if (end == pos) return 0;
    return std::stoull(line.substr(pos, end - pos));
}

}  // namespace

// Faza 16 (§16.4, differentiator from test_storage_failure.cpp): same
// real kill+recovery trigger, but asserts OPERATOR-visible evidence of
// recovery rather than just client-observable READ success — exactly
// what tests/integration/test_recovery.cpp's in-process version cannot
// do, since it never spawns dfs_statsctl as a real subprocess against a
// real Master. Verifies (a) the recovered replica physically exists
// on-disk on a real node, and (b) dfs_statsctl's real wire-protocol
// STATS_RESPONSE against the real Master process shows
// detected_failures/recovery_operations/successful_recoveries actually
// incremented.
class FailureRecoveryStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_failure_recovery_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(FailureRecoveryStatsTest, RecoveryIsVisibleOnDiskAndInRealStatsctlOutput) {
    using namespace dfs_test;

    constexpr uint16_t kMasterPort = 41720;
    constexpr uint16_t kStorage1Port = 41721;
    constexpr uint16_t kStorage2Port = 41722;
    constexpr uint16_t kStorage3Port = 41723;
    constexpr uint16_t kStorage4Port = 41724;  // spare

    auto cluster_opt = spawn_cluster(
        dir_, kMasterPort, {kStorage1Port, kStorage2Port, kStorage3Port, kStorage4Port},
        std::chrono::seconds(5));
    ASSERT_TRUE(cluster_opt.has_value()) << "cluster failed to come up";
    SpawnedCluster cluster = std::move(*cluster_opt);
    ASSERT_EQ(cluster.storage_nodes.size(), 4u);

    std::filesystem::path src = dir_ / "src.txt";
    {
        std::ofstream out(src);
        out << std::string(500, 'y');
    }

    ASSERT_EQ(run_client(kMasterPort, {"create", "/recov.txt"}), 0);
    ASSERT_EQ(run_client(kMasterPort, {"write", "/recov.txt", src.string()}), 0);

    // Baseline: before any failure, the stats must show zero recovery
    // activity — otherwise a later nonzero reading wouldn't actually
    // prove anything.
    std::string baseline_stats = run_statsctl(kMasterPort);
    EXPECT_EQ(extract_stat(baseline_stats, "Failures:", "detected="), 0u);
    EXPECT_EQ(extract_stat(baseline_stats, "Recovery:", "operations="), 0u);

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

    // Poll the REAL Master process's REAL wire-protocol stats output
    // (not an in-process accessor) until it reports the failure was
    // detected and recovery succeeded.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    bool recovered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string stats = run_statsctl(kMasterPort);
        if (extract_stat(stats, "Failures:", "detected=") >= 1 &&
            extract_stat(stats, "Recovery:", "successful=") >= 1) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_TRUE(recovered) << "dfs_statsctl never reported a completed recovery";

    // Physical, operator-visible evidence: exactly 3 real on-disk
    // replicas remain (the 2 original survivors plus the spare, now
    // holding a fresh copy).
    int replicas_on_disk = 0;
    for (size_t i = 0; i < cluster.storage_data_dirs.size(); ++i) {
        if (i != killed && has_any_block_on_disk(cluster.storage_data_dirs[i])) {
            ++replicas_on_disk;
        }
    }
    EXPECT_EQ(replicas_on_disk, 3);

    cluster.stop_all();
}
