#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "client/client.h"
#include "process_cluster.h"

// Faza 17.5 — Eksperiment 5 (design notes §10.37): Storage Node failure.
// Measures failure detection time, recovery duration, bytes recovered,
// and final replication state after a real SIGKILL on a replica holder
// (RF=3, default — this experiment is about the failure/recovery
// MECHANISM itself, not an RF comparison; that was Faza 17.4's
// Experiment 4).
//
// "Recovery start time" (one of §10.37's five bullets) is not separately
// instrumented in StatisticsManager and isn't reported as its own
// number: RecoveryManager::handle_node_failure()
// (src/master/recovery_manager.cpp) enqueues every affected block's
// recovery job SYNCHRONOUSLY inside the same FailureDetector callback
// that increments detected_failures — there is no observable gap this
// design produces between "detected" and "recovery attempt begins" to
// measure separately. Reported instead: failure detection time (kill ->
// Master's detected_failures first becomes >=1) and recovery duration
// (detection -> successful_recoveries first becomes >=1) — together
// these cover the same kill-to-recovered timeline, just without an
// artificial "start" milestone.
namespace {

constexpr uint16_t kMasterPort = 41850;
constexpr std::array<uint16_t, 4> kStoragePorts = {41851, 41852, 41853, 41854};

// Two sizes, small enough to keep each ~15-20s detection+recovery cycle
// from being dominated by data-transfer time, large enough that
// bytes_recovered visibly scales between them.
constexpr std::array<uint64_t, 2> kFileSizes = {1ULL * 1024 * 1024, 10ULL * 1024 * 1024};
constexpr auto kPollInterval = std::chrono::milliseconds(500);
// Same budget as tests/failure/test_storage_failure.cpp and Faza 17.4's
// RF=1 sub-run — real SUSPECTED_TIMEOUT_SECONDS(6)+DEAD_TIMEOUT_SECONDS(12)
// detection plus real recovery, not shortened (see docs/design.md §20).
constexpr auto kDeadline = std::chrono::seconds(40);

struct NodeFailureResult {
    uint64_t file_size;
    bool detected;
    double detection_seconds;
    bool recovered;
    double recovery_duration_seconds;
    uint64_t bytes_recovered;
    int final_replica_count;
};

NodeFailureResult run_node_failure_experiment(uint64_t file_size) {
    using namespace dfs_bench;
    using namespace dfs_test;

    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                 ("dfs_bench_node_failure_" + std::to_string(file_size));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::vector<uint16_t> storage_ports(kStoragePorts.begin(), kStoragePorts.end());
    auto cluster_opt = spawn_cluster(dir, kMasterPort, storage_ports, std::chrono::seconds(5));
    if (!cluster_opt.has_value()) fail("cluster failed to come up");
    SpawnedCluster cluster = std::move(*cluster_opt);
    register_cluster_for_cleanup(&cluster);

    const std::string path = "/node_failure.bin";
    std::vector<uint8_t> payload =
        make_payload(file_size, /*seed=*/4000u + static_cast<uint32_t>(file_size));
    {
        Client::Config config{"127.0.0.1", kMasterPort};
        auto client_result = Client::create(config);
        if (client_result.is_error()) fail("failed to create setup client");
        auto client = std::move(client_result.value());
        if (client->create_file(path) != ErrorCode::OK) fail("CREATE failed");
        if (client->write(path, payload) != ErrorCode::OK) fail("WRITE failed");
    }

    std::vector<size_t> holders;
    for (size_t i = 0; i < cluster.storage_data_dirs.size(); ++i) {
        if (has_any_block_on_disk(cluster.storage_data_dirs[i])) holders.push_back(i);
    }
    if (holders.size() != 3) fail("unexpected initial replica count before kill");
    size_t killed = holders[0];

    if (cluster.storage_nodes[killed].send_signal(SIGKILL) != ErrorCode::OK) fail("SIGKILL failed");
    cluster.storage_nodes[killed].wait_for_exit(std::chrono::seconds(2));
    auto kill_time = std::chrono::steady_clock::now();

    bool detected = false;
    double detection_seconds = 0.0;
    bool recovered = false;
    double recovery_duration_seconds = 0.0;
    uint64_t bytes_recovered = 0;

    auto deadline = kill_time + kDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string stats_text = run_statsctl(kMasterPort);
        uint64_t detected_failures = parse_stat_u64(stats_text, "Failures:", "detected=");
        uint64_t successful_recoveries = parse_stat_u64(stats_text, "Recovery:", "successful=");
        bytes_recovered = parse_stat_u64(stats_text, "Recovery:", "bytes=");

        auto now = std::chrono::steady_clock::now();
        if (!detected && detected_failures >= 1) {
            detected = true;
            detection_seconds = std::chrono::duration<double>(now - kill_time).count();
        }
        if (detected && successful_recoveries >= 1) {
            recovered = true;
            recovery_duration_seconds =
                std::chrono::duration<double>(now - kill_time).count() - detection_seconds;
            break;
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    int final_replica_count = 0;
    for (size_t i = 0; i < cluster.storage_data_dirs.size(); ++i) {
        if (i != killed && has_any_block_on_disk(cluster.storage_data_dirs[i])) ++final_replica_count;
    }

    cluster.stop_all();
    register_cluster_for_cleanup(nullptr);

    return NodeFailureResult{
        file_size, detected, detection_seconds, recovered, recovery_duration_seconds,
        bytes_recovered, final_replica_count,
    };
}

}  // namespace

int main() {
    using namespace dfs_bench;

    std::vector<TableRow> rows;
    for (uint64_t file_size : kFileSizes) {
        NodeFailureResult result = run_node_failure_experiment(file_size);
        rows.push_back({
            format_mb(static_cast<double>(result.file_size)),
            result.detected ? format_seconds(result.detection_seconds) : "N/A (nije detektovano)",
            result.recovered ? format_seconds(result.recovery_duration_seconds) : "N/A (nije oporavljeno)",
            format_mb(static_cast<double>(result.bytes_recovered)),
            std::to_string(result.final_replica_count) + "/3",
        });
    }

    print_markdown_table(
        "Eksperiment 5 — Storage Node Failure",
        {"Veličina fajla", "Failure detection vrijeme", "Recovery trajanje", "Bytes recovered",
         "Finalno replication stanje"},
        rows);

    return 0;
}
