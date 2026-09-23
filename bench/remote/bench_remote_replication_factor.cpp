#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "client/client.h"
#include "process_cluster.h"
#include "remote_common.h"

// Real-VM mirror of ../bench_replication_factor.cpp. Unlike the loopback
// version, this binary talks to ONE already-running cluster of a fixed,
// externally-provisioned replication_factor (see --rf, a label only —
// replication_factor can't be changed at runtime, see
// remote_common.h's comment) — the calling script (run_benchmarks_vm.sh)
// runs this binary twice total, once against an RF=1 cluster and once
// against an RF=3 cluster, and the two invocations' single-row outputs are
// concatenated into one table.
//
// Phase A (write + storage) needs no process restarts between repeats:
// each repeat writes a uniquely-named batch of files, so the storage-used
// delta it measures is still exactly that repeat's contribution even
// though the cluster (and its cumulative used_bytes) stays up across all 3
// repeats — unlike the loopback version, which restarts the whole cluster
// per repeat purely to get a clean disk for this same reason.
//
// Phase B (recovery) DOES need a real per-repeat reset, because it's
// destructive (kills a live storage VM's process): the killed node's
// process no longer exists afterward, so vm_cluster.sh's restart_storage
// wipes that node's data dir and starts it fresh before the next repeat's
// write. Master's detected_failures/successful_recoveries counters are
// cumulative for the cluster's whole lifetime here (no fresh Master per
// repeat like loopback) — every repeat therefore snapshots a BASELINE
// before killing, and looks for the counters to advance PAST that
// baseline, not for an absolute value >= 1.
//
// "holders" (which storage nodes actually received a replica) can't be
// read off local disk here (the nodes are remote VMs) — instead, each
// storage node's `Storage: ... used_bytes=` GET_STATS line is diffed
// before/after the write, exactly like Phase A's storage measurement; any
// node whose used_bytes grew by kFileSize is a holder. This is the
// network-visible equivalent of ../bench_replication_factor.cpp's
// has_any_block_on_disk() filesystem check.
namespace {

constexpr uint64_t kFileSize = 1ULL * 1024 * 1024;  // 1 MiB, 1 block/file
constexpr int kNumFiles = 20;
constexpr auto kRecoveryDeadline = std::chrono::seconds(40);
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

uint64_t sum(const std::vector<uint64_t>& values) {
    uint64_t total = 0;
    for (uint64_t v : values) total += v;
    return total;
}

struct WriteStorageResult {
    dfs_bench::LatencyStats write_latency;
    uint64_t storage_measured_bytes;
};

WriteStorageResult run_write_storage_repeat(const RemoteCluster& cluster, int rep) {
    using namespace dfs_bench;

    uint64_t used_before = sum(per_storage_used_bytes(cluster));

    Client::Config config{cluster.master.host, cluster.master.port};
    auto client_result = Client::create(config);
    if (client_result.is_error()) fail("failed to create write client");
    auto client = std::move(client_result.value());

    std::vector<uint64_t> latencies;
    for (int i = 0; i < kNumFiles; ++i) {
        std::string path =
            "/vm_rf_write_r" + std::to_string(rep) + "_" + std::to_string(i) + ".bin";
        std::vector<uint8_t> payload =
            make_payload(kFileSize, /*seed=*/2000u + static_cast<uint32_t>(rep * kNumFiles + i));
        if (client->create_file(path) != ErrorCode::OK) fail("CREATE failed");

        auto start = std::chrono::steady_clock::now();
        ErrorCode result = client->write(path, payload);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        if (result != ErrorCode::OK) fail("WRITE failed during benchmark");
        latencies.push_back(static_cast<uint64_t>(elapsed.count()));
    }

    uint64_t used_after = sum(per_storage_used_bytes(cluster));
    if (used_after < used_before) fail("storage usage decreased unexpectedly — flaky GET_STATS?");

    return WriteStorageResult{compute_latency_stats(std::move(latencies)), used_after - used_before};
}

struct RecoveryResult {
    bool recovered;
    double seconds_to_outcome;
};

RecoveryResult run_recovery_repeat(const RemoteCluster& cluster, uint32_t rf, int rep) {
    using namespace dfs_bench;

    const std::string path = "/vm_rf_recovery_r" + std::to_string(rep) + ".bin";
    std::vector<uint8_t> payload = make_payload(kFileSize, /*seed=*/3000u + rf * 100u + rep);

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
    if (holders.size() != rf) fail("unexpected initial replica count before kill (network-visible)");
    size_t killed = holders[0];

    std::string baseline_stats = dfs_test::run_statsctl(cluster.master.port, cluster.master.host);
    uint64_t detected_baseline = parse_stat_u64(baseline_stats, "Failures:", "detected=");
    uint64_t successful_baseline = parse_stat_u64(baseline_stats, "Recovery:", "successful=");

    dfs_bench_remote::kill_remote_storage(cluster, killed);

    auto kill_time = std::chrono::steady_clock::now();
    auto deadline = kill_time + kRecoveryDeadline;
    bool recovered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string stats_text = dfs_test::run_statsctl(cluster.master.port, cluster.master.host);
        uint64_t successful_now = parse_stat_u64(stats_text, "Recovery:", "successful=");
        // RF=1 has no surviving source replica to recover from (see
        // ../bench_replication_factor.cpp's identical rationale) — this
        // loop times out at kRecoveryDeadline for RF=1 by construction.
        if (successful_now > successful_baseline) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    double seconds_to_outcome =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - kill_time).count();

    // Refresh the killed node's process/data before the next repeat writes
    // through it again, regardless of whether recovery succeeded — then
    // wait out the reconciliation churn that refresh itself triggers (see
    // remote_common.h's wait_for_storage_quiescent() doc comment) so it
    // doesn't get misattributed to the NEXT repeat's write delta.
    dfs_bench_remote::restart_remote_storage(cluster, killed);
    dfs_bench_remote::wait_for_storage_quiescent(cluster);

    (void)detected_baseline;  // kept for symmetry with loopback's row shape; not tabulated separately here
    return RecoveryResult{recovered, seconds_to_outcome};
}

}  // namespace

int main(int argc, char** argv) {
    using namespace dfs_bench;
    using dfs_bench_remote::parse_remote_args;

    auto cluster = parse_remote_args(argc, argv);
    uint32_t rf = cluster.replication_factor_label;
    if (rf == 0) fail("--rf N is required (label for the already-provisioned cluster's RF)");

    std::vector<double> avg_us_reps, p95_us_reps, storage_reps;
    for (int rep = 0; rep < kRepeats; ++rep) {
        WriteStorageResult result = run_write_storage_repeat(cluster, rep);
        avg_us_reps.push_back(result.write_latency.avg_us);
        p95_us_reps.push_back(static_cast<double>(result.write_latency.p95_us));
        storage_reps.push_back(static_cast<double>(result.storage_measured_bytes));
    }
    double total_logical_bytes = static_cast<double>(kFileSize) * static_cast<double>(kNumFiles);
    double network_estimate_bytes = total_logical_bytes * static_cast<double>(rf);
    Aggregate latency_agg = aggregate_values(avg_us_reps);
    Aggregate p95_agg = aggregate_values(p95_us_reps);
    Aggregate storage_agg = aggregate_values(storage_reps);

    print_markdown_table(
        "Eksperiment 4a (VM) — Replication Factor: write latencija i storage potrošnja "
        "(3 ponavljanja, min / mean / max) — RF=" + std::to_string(rf),
        {"RF", "Write latencija min/mean/max", "P95 latencija min/mean/max", "Ukupno (logički)",
         "Network estimate (tačno)", "Storage izmjereno (mean, per-repeat delta)"},
        {{std::to_string(rf), format_min_mean_max_millis(latency_agg),
          format_min_mean_max_millis(p95_agg), format_mb(total_logical_bytes),
          format_mb(network_estimate_bytes), format_mb(storage_agg.mean)}});

    // Phase A never kills/wipes a node, but confirm the cluster is at rest
    // before Phase B's first baseline snapshot anyway — cheap, and Phase B's
    // single-write deltas are far more sensitive to any leftover churn than
    // Phase A's 20-file-batch sums were.
    dfs_bench_remote::wait_for_storage_quiescent(cluster);

    std::vector<double> seconds_reps;
    int recovered_count = 0;
    for (int rep = 0; rep < kRepeats; ++rep) {
        RecoveryResult result = run_recovery_repeat(cluster, rf, rep);
        seconds_reps.push_back(result.seconds_to_outcome);
        if (result.recovered) ++recovered_count;
    }
    Aggregate seconds_agg = aggregate_values(seconds_reps);

    print_markdown_table(
        "Eksperiment 4b (VM) — Replication Factor: recovery ponašanje "
        "(3 ponavljanja, min / mean / max) — RF=" + std::to_string(rf),
        {"RF", "Ishod (koliko od N ponavljanja oporavljeno)", "Vrijeme do ishoda min/mean/max"},
        {{std::to_string(rf),
          std::to_string(recovered_count) + "/" + std::to_string(kRepeats) + " OPORAVLJENO",
          format_min_mean_max_seconds(seconds_agg)}});

    return 0;
}
