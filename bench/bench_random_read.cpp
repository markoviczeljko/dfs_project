#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "client/client.h"
#include "process_cluster.h"

// Faza 17.3 — Eksperiment 2 (design notes §10.37): random block reads,
// measuring latency + cache hit ratio.
//
// Client's public API only exposes whole-file read() (see
// include/client/client.h) — there is no partial/range READ in the wire
// protocol. "Random block reads" is therefore operationalized here as:
// kNumFiles distinct single-block files, each client thread repeatedly
// reading a uniformly-random one of them — the block fetched next is
// random, even though each individual call is a whole-file read() (see
// docs/design.md §23 for why this is the faithful implementation of
// §10.37 Experiment 2 given the actual Client API surface).
//
// A small Client::Config::cache_max_bytes (kCacheBytes, a handful of
// blocks) against a working set larger than the cache (kNumFiles) is
// what makes the hit ratio measurement meaningful — with the default
// 512 MB cache and only a few small files, every read after the first
// would trivially hit, telling us nothing about cache behavior.
namespace {

constexpr uint16_t kMasterPort = 41810;
constexpr uint16_t kStorage1Port = 41811;
constexpr uint16_t kStorage2Port = 41812;
constexpr uint16_t kStorage3Port = 41813;

constexpr uint64_t kBlockSize = 1ULL * 1024 * 1024;   // 1 MiB per file/block
constexpr uint64_t kCacheBytes = 4ULL * 1024 * 1024;  // fits ~4 blocks
constexpr int kNumFiles = 20;                         // working set > cache
constexpr int kReadsPerClient = 30;

// Each client_count cell is run this many times independently so results
// show run-to-run variance (min/mean/max), not a single run's numbers.
constexpr int kRepeats = 3;

// §16.8, reduced for this sandboxed validation run — see
// bench_sequential_read.cpp's identical note (docs/design.md §23).
constexpr std::array<int, 3> kClientCounts = {1, 5, 10};

}  // namespace

int main() {
    using namespace dfs_bench;
    using namespace dfs_test;

    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dfs_bench_random_read";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto cluster_opt = spawn_cluster(dir, kMasterPort, {kStorage1Port, kStorage2Port, kStorage3Port},
                                      std::chrono::seconds(5));
    if (!cluster_opt.has_value()) fail("cluster failed to come up");
    SpawnedCluster cluster = std::move(*cluster_opt);
    register_cluster_for_cleanup(&cluster);

    std::vector<std::string> paths;
    {
        Client::Config setup_config{"127.0.0.1", kMasterPort, kBlockSize};
        auto setup_result = Client::create(setup_config);
        if (setup_result.is_error()) fail("failed to create setup client");
        auto setup_client = std::move(setup_result.value());
        for (int i = 0; i < kNumFiles; ++i) {
            std::string path = "/randread_" + std::to_string(i) + ".bin";
            std::vector<uint8_t> payload = make_payload(kBlockSize, /*seed=*/1000u + static_cast<uint32_t>(i));
            if (setup_client->create_file(path) != ErrorCode::OK) fail("CREATE failed");
            if (setup_client->write(path, payload) != ErrorCode::OK) fail("WRITE failed");
            paths.push_back(std::move(path));
        }
    }

    std::vector<TableRow> rows;

    for (int client_count : kClientCounts) {
        std::vector<uint64_t> pooled_latencies;
        std::vector<double> avg_us_per_rep;
        std::vector<double> throughput_per_rep;
        std::vector<double> hit_ratio_per_rep;

        for (int rep = 0; rep < kRepeats; ++rep) {
            std::vector<std::vector<uint64_t>> per_thread_latencies(static_cast<size_t>(client_count));
            std::vector<uint64_t> per_thread_hits(static_cast<size_t>(client_count), 0);
            std::vector<uint64_t> per_thread_misses(static_cast<size_t>(client_count), 0);
            std::vector<std::thread> threads;

            auto wall_start = std::chrono::steady_clock::now();
            for (int t = 0; t < client_count; ++t) {
                threads.emplace_back([&, t] {
                    Client::Config config{"127.0.0.1", kMasterPort, kBlockSize, kCacheBytes};
                    auto client_result = Client::create(config);
                    if (client_result.is_error()) fail("failed to create read client");
                    auto client = std::move(client_result.value());

                    // rep folded into the per-thread seed so each repetition draws a
                    // different (but still reproducible) sequence of offsets rather
                    // than replaying the exact same access pattern three times.
                    std::mt19937 rng(static_cast<uint32_t>((rep * client_count) + t + 1));
                    std::uniform_int_distribution<int> pick(0, kNumFiles - 1);
                    for (int r = 0; r < kReadsPerClient; ++r) {
                        const std::string& path = paths[static_cast<size_t>(pick(rng))];
                        auto start = std::chrono::steady_clock::now();
                        auto read_result = client->read(path);
                        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start);
                        if (read_result.is_error() || read_result.value().size() != kBlockSize) {
                            fail("READ returned wrong data/error during benchmark");
                        }
                        per_thread_latencies[static_cast<size_t>(t)].push_back(
                            static_cast<uint64_t>(elapsed.count()));
                    }

                    CacheStats cache_stats = client->cache_stats();
                    per_thread_hits[static_cast<size_t>(t)] = cache_stats.hits;
                    per_thread_misses[static_cast<size_t>(t)] = cache_stats.misses;
                });
            }
            for (std::thread& th : threads) th.join();
            double wall_elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();

            std::vector<uint64_t> rep_latencies;
            uint64_t total_hits = 0;
            uint64_t total_misses = 0;
            for (int t = 0; t < client_count; ++t) {
                auto& thread_samples = per_thread_latencies[static_cast<size_t>(t)];
                rep_latencies.insert(rep_latencies.end(), thread_samples.begin(), thread_samples.end());
                total_hits += per_thread_hits[static_cast<size_t>(t)];
                total_misses += per_thread_misses[static_cast<size_t>(t)];
            }
            LatencyStats rep_stats = compute_latency_stats(rep_latencies);
            double total_bytes = static_cast<double>(kBlockSize) * static_cast<double>(kReadsPerClient) *
                                  static_cast<double>(client_count);
            double hit_ratio = (total_hits + total_misses) > 0
                                    ? static_cast<double>(total_hits) /
                                          static_cast<double>(total_hits + total_misses)
                                    : 0.0;

            avg_us_per_rep.push_back(rep_stats.avg_us);
            throughput_per_rep.push_back(total_bytes / (1024.0 * 1024.0) / wall_elapsed);
            hit_ratio_per_rep.push_back(hit_ratio);
            pooled_latencies.insert(pooled_latencies.end(), rep_latencies.begin(), rep_latencies.end());
        }

        LatencyStats pooled_stats = compute_latency_stats(pooled_latencies);
        Aggregate latency_agg = aggregate_values(avg_us_per_rep);
        Aggregate throughput_agg = aggregate_values(throughput_per_rep);
        Aggregate hit_ratio_agg = aggregate_values(hit_ratio_per_rep);

        rows.push_back({
            std::to_string(client_count),
            format_min_mean_max_mb_s(throughput_agg),
            format_min_mean_max_millis(latency_agg),
            format_millis(static_cast<double>(pooled_stats.p95_us)),
            format_percent(hit_ratio_agg.min) + " / " + format_percent(hit_ratio_agg.mean) + " / " +
                format_percent(hit_ratio_agg.max),
        });
    }

    print_markdown_table(
        "Eksperiment 2 — Random READ (3 ponavljanja po ćeliji, min / mean / max)",
        {"Broj klijenata", "Throughput min/mean/max (MB/s)", "Latencija min/mean/max",
         "P95 latencija (pooled)", "Cache hit ratio min/mean/max"},
        rows);

    cluster.stop_all();
    return 0;
}
