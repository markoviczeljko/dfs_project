#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "client/client.h"
#include "process_cluster.h"
#include "remote_common.h"

// Real-VM mirror of ../bench_node_failure.cpp. Assumes a fixed RF=3
// cluster is already provisioned and running (see docs/benchmark_results_vm.md's
// methodology header) — this experiment is about the failure/recovery
// MECHANISM, not an RF comparison, exactly like the loopback original.
//
// Two adaptations versus the loopback version, both because this binary
// only ever sees the cluster over the network, never its disks or process
// table directly (see bench_remote_replication_factor.cpp's header comment
// for the same rationale applied there):
//  - "holders" (which nodes got a replica) and the final replica count are
//    inferred from per-storage `used_bytes` GET_STATS deltas, not
//    has_any_block_on_disk().
//  - Master's detected_failures/successful_recoveries/bytes counters are
//    cumulative for the whole (persistent) cluster session, not reset per
//    repeat — every repeat snapshots a baseline before killing and reports
//    the counters' advance PAST that baseline, not an absolute value.
namespace {

constexpr std::array<uint64_t, 2> kFileSizes = {1ULL * 1024 * 1024, 10ULL * 1024 * 1024};
constexpr std::chrono::milliseconds kPollInterval(500);
constexpr auto kDeadline = std::chrono::seconds(40);
constexpr int kRepeats = 3;

using dfs_bench_remote::RemoteCluster;

std::vector<uint64_t> per_storage_used_bytes(const RemoteCluster& cluster) {
    std::vector<uint64_t> result;
    for (const auto& node : cluster.storage) {
        std::string stats_text = dfs_test::run_statsctl(node.port, node.host);
        result.push_back(dfs_bench::parse_stat_u64(stats_text, "Storage:", "used_bytes="));
    }
    return result;
}

struct NodeFailureResult {
    bool detected;
    double detection_seconds;
    bool recovered;
    double recovery_duration_seconds;
    uint64_t bytes_recovered;
};

NodeFailureResult run_repeat(const RemoteCluster& cluster, uint64_t file_size, int rep) {
    using namespace dfs_bench;

    const std::string path = "/vm_node_failure_r" + std::to_string(rep) + "_" +
                              std::to_string(file_size) + ".bin";
    std::vector<uint8_t> payload =
        make_payload(file_size, /*seed=*/4000u + static_cast<uint32_t>(file_size) + rep);

    std::vector<uint64_t> before = per_storage_used_bytes(cluster);
    {
        Client::Config config{cluster.master.host, cluster.master.port};
        auto client_result = Client::create(config);
        if (client_result.is_error()) fail("failed to create setup client");
        auto client = std::move(client_result.value());
        if (client->create_file(path) != ErrorCode::OK) fail("CREATE failed");
        if (client->write(path, payload) != ErrorCode::OK) fail("WRITE failed");
    }
    std::vector<uint64_t> after = per_storage_used_bytes(cluster);

    std::vector<size_t> holders;
    for (size_t i = 0; i < cluster.storage.size(); ++i) {
        if (after[i] > before[i]) holders.push_back(i);
    }
    if (holders.size() != 3) fail("unexpected initial replica count before kill (network-visible)");
    size_t killed = holders[0];

    std::string baseline_stats = dfs_test::run_statsctl(cluster.master.port, cluster.master.host);
    uint64_t detected_baseline = parse_stat_u64(baseline_stats, "Failures:", "detected=");
    uint64_t successful_baseline = parse_stat_u64(baseline_stats, "Recovery:", "successful=");
    uint64_t bytes_baseline = parse_stat_u64(baseline_stats, "Recovery:", "bytes=");

    dfs_bench_remote::kill_remote_storage(cluster, killed);
    auto kill_time = std::chrono::steady_clock::now();

    bool detected = false;
    double detection_seconds = 0.0;
    bool recovered = false;
    double recovery_duration_seconds = 0.0;
    uint64_t bytes_recovered = 0;

    auto deadline = kill_time + kDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string stats_text = dfs_test::run_statsctl(cluster.master.port, cluster.master.host);
        uint64_t detected_now = parse_stat_u64(stats_text, "Failures:", "detected=");
        uint64_t successful_now = parse_stat_u64(stats_text, "Recovery:", "successful=");
        uint64_t bytes_now = parse_stat_u64(stats_text, "Recovery:", "bytes=");

        auto now = std::chrono::steady_clock::now();
        if (!detected && detected_now > detected_baseline) {
            detected = true;
            detection_seconds = std::chrono::duration<double>(now - kill_time).count();
        }
        if (detected && successful_now > successful_baseline) {
            recovered = true;
            recovery_duration_seconds =
                std::chrono::duration<double>(now - kill_time).count() - detection_seconds;
            bytes_recovered = bytes_now - bytes_baseline;
            break;
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    // See bench_remote_replication_factor.cpp's identical comment: the
    // wipe-and-rejoin above triggers legitimate Master-driven
    // re-replication for this node's ENTIRE prior block history, which
    // must settle before the next repeat's delta measurement.
    dfs_bench_remote::restart_remote_storage(cluster, killed);
    dfs_bench_remote::wait_for_storage_quiescent(cluster);

    return NodeFailureResult{detected, detection_seconds, recovered, recovery_duration_seconds,
                              bytes_recovered};
}

}  // namespace

int main(int argc, char** argv) {
    using namespace dfs_bench;
    using dfs_bench_remote::parse_remote_args;

    auto cluster = parse_remote_args(argc, argv);

    std::vector<TableRow> rows;
    for (uint64_t file_size : kFileSizes) {
        std::vector<double> detection_reps, recovery_reps, bytes_reps;
        int detected_count = 0;
        int recovered_count = 0;
        for (int rep = 0; rep < kRepeats; ++rep) {
            NodeFailureResult result = run_repeat(cluster, file_size, rep);
            if (result.detected) {
                ++detected_count;
                detection_reps.push_back(result.detection_seconds);
            }
            if (result.recovered) {
                ++recovered_count;
                recovery_reps.push_back(result.recovery_duration_seconds);
                bytes_reps.push_back(static_cast<double>(result.bytes_recovered));
            }
        }
        Aggregate detection_agg = aggregate_values(detection_reps);
        Aggregate recovery_agg = aggregate_values(recovery_reps);
        Aggregate bytes_agg = aggregate_values(bytes_reps);

        rows.push_back({
            format_mb(static_cast<double>(file_size)),
            detected_count > 0
                ? format_min_mean_max_seconds(detection_agg) + " (" + std::to_string(detected_count) +
                      "/" + std::to_string(kRepeats) + ")"
                : "N/A (nije detektovano)",
            recovered_count > 0
                ? format_min_mean_max_seconds(recovery_agg) + " (" + std::to_string(recovered_count) +
                      "/" + std::to_string(kRepeats) + ")"
                : "N/A (nije oporavljeno)",
            recovered_count > 0 ? format_mb(bytes_agg.mean) : "N/A",
        });
    }

    print_markdown_table(
        "Eksperiment 5 (VM) — Storage Node Failure (3 ponavljanja po veličini fajla, min / mean / max)",
        {"Veličina fajla", "Failure detection vrijeme", "Recovery trajanje",
         "Bytes recovered (mean)"},
        rows);

    return 0;
}
