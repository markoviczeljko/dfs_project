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

// Faza 17.4 — Eksperiment 4 (design notes §10.37): replication factor
// comparison (RF=1 vs RF=3). Measures write latency, storage
// consumption, and recovery behavior. replication_factor is Faza 17.1's
// runtime master.conf key, fixed for a Master's whole lifetime (see
// docs/design.md §21) — so each RF value gets its own sequential
// Master+cluster run rather than a live toggle.
//
// Network traffic for WRITE is computed analytically, not approximated
// via OperationStats.bytes (which records only the logical file size,
// identical regardless of RF — see src/client/client.cpp's write() and
// docs/design.md §24's identical finding for READ): write_impl() sends
// the full chunk to EVERY replica in the list
// (write_chunk_to_replicas()), so outbound network bytes = logical_bytes
// * replication_factor exactly, for this deterministic single-writer
// workload — no instrumentation needed, this is exact, not an estimate.
//
// Storage consumption IS measured directly, not computed: each Storage
// Node's own `Storage: ... used_bytes=` GET_STATS line
// (src/storage/storage_node.cpp's handle_get_stats() recomputes it
// synchronously from BlockManager on every request — no staleness to
// wait out) is summed across all 4 nodes, before and after the write
// phase.
namespace {

// Each of these two port sets is reused, unchanged, across both
// sequential RF=1/RF=3 sub-runs of its phase (unlike e.g.
// tests/failure/test_replication_factor_config.cpp's two disjoint port
// blocks for its two test cases) — intentional, not an oversight:
// ProcessHandle's destructor (tests/failure/process_cluster.cpp) SIGKILLs
// and reaps as a hard safety net before run_write_storage_phase()/
// run_recovery_phase() returns, so the previous run's processes are
// guaranteed dead before the next spawn_cluster() call on the same ports.
constexpr uint16_t kMasterPortA = 41830;  // Phase A: write latency + storage
constexpr std::array<uint16_t, 4> kStoragePortsA = {41831, 41832, 41833, 41834};

constexpr uint16_t kMasterPortB = 41840;  // Phase B: recovery behavior
constexpr std::array<uint16_t, 4> kStoragePortsB = {41841, 41842, 41843, 41844};

constexpr uint64_t kFileSize = 1ULL * 1024 * 1024;  // 1 MiB, 1 block/file
constexpr int kNumFiles = 20;
constexpr std::array<uint32_t, 2> kReplicationFactors = {1, 3};
// Same 40s budget as tests/failure/test_storage_failure.cpp — real
// SUSPECTED_TIMEOUT_SECONDS(6)+DEAD_TIMEOUT_SECONDS(12) failure detection
// plus real recovery, not shortened (see docs/design.md §20). For RF=1
// this deadline is never met early: recovery structurally cannot
// succeed (see below), so that sub-run always costs the full 40s.
constexpr auto kRecoveryDeadline = std::chrono::seconds(40);

// Both phases are run this many times per RF value so results show
// run-to-run variance (min/mean/max), not a single run's numbers.
constexpr int kRepeats = 3;

uint64_t sum_storage_used_bytes(const std::array<uint16_t, 4>& storage_ports) {
    uint64_t total = 0;
    for (uint16_t port : storage_ports) {
        std::string stats_text = dfs_test::run_statsctl(port);
        total += dfs_bench::parse_stat_u64(stats_text, "Storage:", "used_bytes=");
    }
    return total;
}

struct WriteStorageResult {
    uint32_t replication_factor;
    dfs_bench::LatencyStats write_latency;
    double total_logical_bytes;
    double network_estimate_bytes;
    double storage_measured_bytes;
};

WriteStorageResult run_write_storage_phase(uint32_t rf) {
    using namespace dfs_bench;
    using namespace dfs_test;

    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("dfs_bench_rf_write_" + std::to_string(rf));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::vector<uint16_t> storage_ports(kStoragePortsA.begin(), kStoragePortsA.end());
    auto cluster_opt = spawn_cluster(dir, kMasterPortA, storage_ports, std::chrono::seconds(5), rf);
    if (!cluster_opt.has_value()) fail("Phase A cluster failed to come up");
    SpawnedCluster cluster = std::move(*cluster_opt);
    register_cluster_for_cleanup(&cluster);

    uint64_t used_before = sum_storage_used_bytes(kStoragePortsA);

    Client::Config config{"127.0.0.1", kMasterPortA};
    auto client_result = Client::create(config);
    if (client_result.is_error()) fail("failed to create write client");
    auto client = std::move(client_result.value());

    std::vector<uint64_t> latencies;
    for (int i = 0; i < kNumFiles; ++i) {
        std::string path = "/rf_write_" + std::to_string(i) + ".bin";
        std::vector<uint8_t> payload = make_payload(kFileSize, /*seed=*/2000u + static_cast<uint32_t>(i));
        if (client->create_file(path) != ErrorCode::OK) fail("CREATE failed");

        auto start = std::chrono::steady_clock::now();
        ErrorCode result = client->write(path, payload);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        if (result != ErrorCode::OK) fail("WRITE failed during benchmark");
        latencies.push_back(static_cast<uint64_t>(elapsed.count()));
    }

    uint64_t used_after = sum_storage_used_bytes(kStoragePortsA);
    // A flaky run_statsctl()/parse_stat_u64() call returning 0 for one
    // node in one of the two snapshots (both fail "softly", see
    // bench_common.h's parse_stat_u64 doc comment) would otherwise wrap
    // this unsigned subtraction to a huge value and silently report a
    // wildly wrong "Storage izmjereno" figure instead of failing loudly
    // — flagged during review as the one real gap in an
    // otherwise-clean pass.
    if (used_after < used_before) fail("storage usage decreased unexpectedly — flaky GET_STATS?");
    LatencyStats stats = compute_latency_stats(std::move(latencies));
    double total_logical_bytes = static_cast<double>(kFileSize) * static_cast<double>(kNumFiles);

    cluster.stop_all();
    // Clear the registration before `cluster` goes out of scope — this
    // function is called twice in sequence (RF=1 then RF=3), each with
    // its own local cluster. Without this, the pointer left over from
    // this call would dangle (point at freed stack memory) during the
    // next call's setup, and a spawn_cluster() failure there (before its
    // own register_cluster_for_cleanup() runs) would make fail() call
    // stop_all() through a use-after-free pointer instead of just
    // skipping cleanup (found while reviewing this file's own design
    // — see docs/design.md §24).
    register_cluster_for_cleanup(nullptr);

    return WriteStorageResult{
        rf,
        stats,
        total_logical_bytes,
        total_logical_bytes * static_cast<double>(rf),
        static_cast<double>(used_after - used_before),
    };
}

struct RecoveryResult {
    uint32_t replication_factor;
    bool recovered;
    double seconds_to_outcome;
    uint64_t detected_failures;
    uint64_t successful_recoveries;
};

RecoveryResult run_recovery_phase(uint32_t rf) {
    using namespace dfs_bench;
    using namespace dfs_test;

    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("dfs_bench_rf_recovery_" + std::to_string(rf));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::vector<uint16_t> storage_ports(kStoragePortsB.begin(), kStoragePortsB.end());
    auto cluster_opt = spawn_cluster(dir, kMasterPortB, storage_ports, std::chrono::seconds(5), rf);
    if (!cluster_opt.has_value()) fail("Phase B cluster failed to come up");
    SpawnedCluster cluster = std::move(*cluster_opt);
    register_cluster_for_cleanup(&cluster);

    const std::string path = "/rf_recovery.bin";
    std::vector<uint8_t> payload = make_payload(kFileSize, /*seed=*/3000u + rf);
    {
        Client::Config config{"127.0.0.1", kMasterPortB};
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
    if (holders.size() != rf) fail("unexpected initial replica count before kill");
    size_t killed = holders[0];

    if (cluster.storage_nodes[killed].send_signal(SIGKILL) != ErrorCode::OK) fail("SIGKILL failed");
    cluster.storage_nodes[killed].wait_for_exit(std::chrono::seconds(2));

    auto kill_time = std::chrono::steady_clock::now();
    auto deadline = kill_time + kRecoveryDeadline;
    bool recovered = false;
    // For RF=3 (4 total nodes: 3 replicas + 1 spare): recovery succeeds
    // once a previously-non-holding node picks up a fresh copy, restoring
    // the count back to `rf` among the surviving nodes.
    //
    // For RF=1: `killed` was the ONLY replica holder. RecoveryManager::
    // recover_block() (src/master/recovery_manager.cpp) requires a
    // healthy SOURCE replica to copy bytes FROM before it can pick a
    // destination — with zero surviving replicas, `found_source` is
    // always false, every retry attempt gives up immediately, and
    // record_recovery(success=false) is recorded. This loop therefore
    // times out at kRecoveryDeadline for RF=1 by construction, not by
    // bug — RF=1 has no fault tolerance, which is exactly the point of
    // this comparison.
    while (std::chrono::steady_clock::now() < deadline) {
        int holders_now = 0;
        for (size_t i = 0; i < cluster.storage_data_dirs.size(); ++i) {
            if (i != killed && has_any_block_on_disk(cluster.storage_data_dirs[i])) ++holders_now;
        }
        if (static_cast<uint32_t>(holders_now) == rf) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    double seconds_to_outcome =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - kill_time).count();

    std::string stats_text = run_statsctl(kMasterPortB);
    uint64_t detected = parse_stat_u64(stats_text, "Failures:", "detected=");
    uint64_t successful = parse_stat_u64(stats_text, "Recovery:", "successful=");

    cluster.stop_all();
    // See run_write_storage_phase()'s identical comment — this function
    // is also called twice in sequence (RF=1 then RF=3).
    register_cluster_for_cleanup(nullptr);

    return RecoveryResult{rf, recovered, seconds_to_outcome, detected, successful};
}

}  // namespace

int main() {
    using namespace dfs_bench;

    std::vector<TableRow> write_rows;
    for (uint32_t rf : kReplicationFactors) {
        std::vector<double> avg_us_reps, p95_us_reps, storage_reps;
        for (int rep = 0; rep < kRepeats; ++rep) {
            WriteStorageResult result = run_write_storage_phase(rf);
            avg_us_reps.push_back(result.write_latency.avg_us);
            p95_us_reps.push_back(static_cast<double>(result.write_latency.p95_us));
            storage_reps.push_back(result.storage_measured_bytes);
        }
        // Logical/network-estimate bytes are exact analytical values derived
        // from kFileSize/kNumFiles/rf (not measurements subject to
        // run-to-run variance), so they're computed directly instead of
        // read off any one repetition's result.
        double total_logical_bytes = static_cast<double>(kFileSize) * static_cast<double>(kNumFiles);
        double network_estimate_bytes = total_logical_bytes * static_cast<double>(rf);
        Aggregate latency_agg = aggregate_values(avg_us_reps);
        Aggregate p95_agg = aggregate_values(p95_us_reps);
        Aggregate storage_agg = aggregate_values(storage_reps);
        write_rows.push_back({
            std::to_string(rf),
            format_min_mean_max_millis(latency_agg),
            format_min_mean_max_millis(p95_agg),
            format_mb(total_logical_bytes),
            format_mb(network_estimate_bytes),
            format_mb(storage_agg.mean),
        });
    }
    print_markdown_table(
        "Eksperiment 4a — Replication Factor: write latencija i storage potrošnja "
        "(3+1 ponavljanja po RF, min / mean / max)",
        {"RF", "Write latencija min/mean/max", "P95 latencija min/mean/max", "Ukupno (logički)",
         "Network estimate (tačno)", "Storage izmjereno (mean)"},
        write_rows);

    std::vector<TableRow> recovery_rows;
    for (uint32_t rf : kReplicationFactors) {
        std::vector<double> seconds_reps;
        int recovered_count = 0;
        uint64_t detected_sum = 0;
        uint64_t successful_sum = 0;
        for (int rep = 0; rep < kRepeats; ++rep) {
            RecoveryResult result = run_recovery_phase(rf);
            seconds_reps.push_back(result.seconds_to_outcome);
            if (result.recovered) ++recovered_count;
            detected_sum += result.detected_failures;
            successful_sum += result.successful_recoveries;
        }
        Aggregate seconds_agg = aggregate_values(seconds_reps);
        recovery_rows.push_back({
            std::to_string(rf),
            std::to_string(recovered_count) + "/" + std::to_string(kRepeats) + " OPORAVLJENO",
            format_min_mean_max_seconds(seconds_agg),
            std::to_string(detected_sum),
            std::to_string(successful_sum),
        });
    }
    print_markdown_table(
        "Eksperiment 4b — Replication Factor: recovery ponašanje "
        "(3 ponavljanja po RF, min / mean / max)",
        {"RF", "Ishod (koliko od N ponavljanja oporavljeno)", "Vrijeme do ishoda min/mean/max",
         "detected_failures (Master, zbir)", "successful_recoveries (Master, zbir)"},
        recovery_rows);

    return 0;
}
