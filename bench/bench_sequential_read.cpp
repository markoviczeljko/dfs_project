#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "client/client.h"
#include "common/config.h"
#include "process_cluster.h"

// Faza 17.3 — Eksperiment 1 (design notes §10.37): sequential READ of a
// large file. Scans file sizes from §16.7 and a concurrent-client-count
// axis from §16.8. Each (size, client_count) cell: ONE shared file is
// created/written once, then `kReadsPerClient` full sequential reads are
// issued per concurrent client thread against that same file — this
// measures how read throughput/latency behave as more clients hit the
// same block concurrently, not per-client isolation.
//
// Each client thread uses its own in-process Client (Faza 17.2's
// decision: linking client/client.h directly rather than spawning
// dfs_client per read, so the read path is measured without a fresh
// process's startup cost polluting every sample — Client isn't
// thread-safe, so one instance per thread is required, not optional).
namespace {

constexpr uint16_t kMasterPort = 41800;
constexpr uint16_t kStorage1Port = 41801;
constexpr uint16_t kStorage2Port = 41802;
constexpr uint16_t kStorage3Port = 41803;

// §16.7. BLOCK_SIZE (64 MB) is a genuine single real-sized block; 2*BLOCK_SIZE
// and 4*BLOCK_SIZE (128 MB, 256 MB) extend the sweep past one block so
// multi-block files are exercised by an actual benchmark, not just by the
// unit/integration test suite (Faza 8's multi-block WRITE/READ coverage).
constexpr std::array<uint64_t, 7> kSizes = {
    1ULL * 1024,        64ULL * 1024,       1ULL * 1024 * 1024, 10ULL * 1024 * 1024,
    BLOCK_SIZE,         2ULL * BLOCK_SIZE,  4ULL * BLOCK_SIZE,
};

// Each (size, client_count) cell is run this many times independently
// (fresh file, fresh client threads, fresh timing) so the results show
// run-to-run variance (min/mean/max) instead of a single run's numbers.
constexpr int kRepeats = 3;

// §16.8 asks for 1/5/10/25/50/100 concurrent clients. Reduced to
// {1, 5, 10} for this sandboxed dev-environment validation run — 100
// concurrent clients times a 64 MB file is multiple GB of loopback
// traffic per cell, fine on real thesis-defense hardware but wasteful to
// re-run repeatedly here. Extend this array for the real thesis run
// (see docs/design.md §23).
constexpr std::array<int, 3> kClientCounts = {1, 5, 10};

// 20, not 3, so P50/P95/P99 (computed from exact sorted samples, see
// bench_common.h) don't collapse onto the same index/value the way they
// would with only a handful of samples per client — loopback throughput
// is fast enough (a full 15-cell run at kReadsPerClient=3 took ~5s) that
// this costs a few tens of seconds, not minutes.
constexpr int kReadsPerClient = 20;

std::string size_label(uint64_t bytes) {
    if (bytes >= 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    if (bytes >= 1024) return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

}  // namespace

int main() {
    using namespace dfs_bench;
    using namespace dfs_test;

    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dfs_bench_sequential_read";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto cluster_opt = spawn_cluster(dir, kMasterPort, {kStorage1Port, kStorage2Port, kStorage3Port},
                                      std::chrono::seconds(5));
    if (!cluster_opt.has_value()) fail("cluster failed to come up");
    SpawnedCluster cluster = std::move(*cluster_opt);
    register_cluster_for_cleanup(&cluster);

    std::vector<TableRow> rows;

    for (uint64_t size : kSizes) {
        std::vector<uint8_t> payload = make_payload(size, /*seed=*/static_cast<uint32_t>(size));
        std::string path = "/seqread_" + std::to_string(size) + ".bin";

        {
            Client::Config setup_config{"127.0.0.1", kMasterPort};
            auto setup_result = Client::create(setup_config);
            if (setup_result.is_error()) fail("failed to create setup client");
            auto setup_client = std::move(setup_result.value());
            if (setup_client->create_file(path) != ErrorCode::OK) fail("CREATE failed");
            if (setup_client->write(path, payload) != ErrorCode::OK) fail("WRITE failed");
        }

        for (int client_count : kClientCounts) {
            std::vector<uint64_t> pooled_latencies;
            std::vector<double> avg_us_per_rep;
            std::vector<double> throughput_per_rep;

            for (int rep = 0; rep < kRepeats; ++rep) {
                std::vector<std::vector<uint64_t>> per_thread_latencies(
                    static_cast<size_t>(client_count));
                std::vector<std::thread> threads;

                auto wall_start = std::chrono::steady_clock::now();
                for (int t = 0; t < client_count; ++t) {
                    threads.emplace_back([&, t] {
                        Client::Config config{"127.0.0.1", kMasterPort};
                        auto client_result = Client::create(config);
                        if (client_result.is_error()) fail("failed to create read client");
                        auto client = std::move(client_result.value());

                        for (int r = 0; r < kReadsPerClient; ++r) {
                            auto start = std::chrono::steady_clock::now();
                            auto read_result = client->read(path);
                            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start);
                            if (read_result.is_error() ||
                                read_result.value().size() != payload.size()) {
                                fail("READ returned wrong data/error during benchmark");
                            }
                            per_thread_latencies[static_cast<size_t>(t)].push_back(
                                static_cast<uint64_t>(elapsed.count()));
                        }
                    });
                }
                for (std::thread& th : threads) th.join();
                double wall_elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - wall_start).count();

                std::vector<uint64_t> rep_latencies;
                for (auto& thread_samples : per_thread_latencies) {
                    rep_latencies.insert(rep_latencies.end(), thread_samples.begin(),
                                          thread_samples.end());
                }
                LatencyStats rep_stats = compute_latency_stats(rep_latencies);
                double total_bytes = static_cast<double>(size) *
                                      static_cast<double>(kReadsPerClient) *
                                      static_cast<double>(client_count);

                avg_us_per_rep.push_back(rep_stats.avg_us);
                throughput_per_rep.push_back(total_bytes / (1024.0 * 1024.0) / wall_elapsed);
                pooled_latencies.insert(pooled_latencies.end(), rep_latencies.begin(),
                                         rep_latencies.end());
            }

            LatencyStats pooled_stats = compute_latency_stats(pooled_latencies);
            Aggregate latency_agg = aggregate_values(avg_us_per_rep);
            // avg_us_per_rep values are already in microseconds; scale mean/min/max
            // through format_min_mean_max_millis's own /1000 division below.
            Aggregate throughput_agg = aggregate_values(throughput_per_rep);

            rows.push_back({
                size_label(size),
                std::to_string(client_count),
                format_min_mean_max_mb_s(throughput_agg),
                format_min_mean_max_millis(latency_agg),
                format_millis(static_cast<double>(pooled_stats.p95_us)),
            });
        }
    }

    print_markdown_table(
        "Eksperiment 1 — Sequential READ (3 ponavljanja po ćeliji, min / mean / max)",
        {"Veličina fajla", "Broj klijenata", "Throughput min/mean/max (MB/s)",
         "Latencija min/mean/max", "P95 latencija (pooled)"},
        rows);

    cluster.stop_all();
    return 0;
}
