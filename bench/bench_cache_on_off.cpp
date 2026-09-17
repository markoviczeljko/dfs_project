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

// Faza 17.4 — Eksperiment 3 (design notes §10.37): cache ON vs OFF.
// Measures READ latency, an approximation of network/disk traffic, and
// cache hit ratio.
//
// Two distinct files are read in strict alternation (A,B,A,B,...) rather
// than hammering a single file — ClientCache::evict_locked() (see
// src/client/cache.cpp) only evicts existing entries when inserting a
// NEW, different block while already at/over capacity; with a single
// repeatedly-read file, the lone cache entry is inserted once (nothing
// to evict against itself) and then never evicted again, so
// cache_max_bytes=0 would accidentally behave like an infinite cache and
// show ~100% hits even with "caching off". Alternating between two files
// makes every cache_max_bytes=0 read a genuine miss (each put() evicts
// the other file's entry before inserting), which is what "cache OFF"
// is supposed to mean (found while implementing this experiment — see
// docs/design.md §24).
//
// Network/disk approximation refines Faza 17.2's generic decision:
// Client::statistics_snapshot()'s OperationStats[READ].bytes records the
// LOGICAL bytes returned to the caller (see src/client/client.cpp's
// read()), identical whether a block came from cache or network — useless
// for an ON/OFF comparison. Instead, every cache MISS is exactly one real
// network fetch + one real Storage-side disk read of one block, so
// `cache_stats().misses * kFileSize` is used as the shared network/disk
// bytes estimate for both dimensions.
namespace {

constexpr uint16_t kMasterPort = 41820;
constexpr uint16_t kStorage1Port = 41821;
constexpr uint16_t kStorage2Port = 41822;
constexpr uint16_t kStorage3Port = 41823;

// Large enough that a real network fetch is measurably slower than a
// cache-hit memcpy (see bench_sequential_read.cpp's 1MB/10MB/64MB rows
// for the same effect).
constexpr uint64_t kFileSize = 10ULL * 1024 * 1024;
constexpr int kReadsPerClient = 20;
constexpr std::array<int, 3> kClientCounts = {1, 5, 10};

struct CacheMode {
    std::string label;
    uint64_t cache_max_bytes;
};

}  // namespace

int main() {
    using namespace dfs_bench;
    using namespace dfs_test;

    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dfs_bench_cache_on_off";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto cluster_opt = spawn_cluster(dir, kMasterPort, {kStorage1Port, kStorage2Port, kStorage3Port},
                                      std::chrono::seconds(5));
    if (!cluster_opt.has_value()) fail("cluster failed to come up");
    SpawnedCluster cluster = std::move(*cluster_opt);
    register_cluster_for_cleanup(&cluster);

    const std::array<std::string, 2> kPaths = {"/cache_a.bin", "/cache_b.bin"};
    {
        Client::Config setup_config{"127.0.0.1", kMasterPort};
        auto setup_result = Client::create(setup_config);
        if (setup_result.is_error()) fail("failed to create setup client");
        auto setup_client = std::move(setup_result.value());
        for (size_t i = 0; i < kPaths.size(); ++i) {
            std::vector<uint8_t> payload = make_payload(kFileSize, /*seed=*/100u + static_cast<uint32_t>(i));
            if (setup_client->create_file(kPaths[i]) != ErrorCode::OK) fail("CREATE failed");
            if (setup_client->write(kPaths[i], payload) != ErrorCode::OK) fail("WRITE failed");
        }
    }

    const std::array<CacheMode, 2> kCacheModes = {
        CacheMode{"OFF", 0ULL},
        CacheMode{"ON", DEFAULT_CLIENT_CACHE_BYTES},
    };

    std::vector<TableRow> rows;

    for (const CacheMode& mode : kCacheModes) {
        for (int client_count : kClientCounts) {
            std::vector<std::vector<uint64_t>> per_thread_latencies(static_cast<size_t>(client_count));
            std::vector<uint64_t> per_thread_hits(static_cast<size_t>(client_count), 0);
            std::vector<uint64_t> per_thread_misses(static_cast<size_t>(client_count), 0);
            std::vector<std::thread> threads;

            auto wall_start = std::chrono::steady_clock::now();
            for (int t = 0; t < client_count; ++t) {
                threads.emplace_back([&, t] {
                    Client::Config config{"127.0.0.1", kMasterPort, BLOCK_SIZE, mode.cache_max_bytes};
                    auto client_result = Client::create(config);
                    if (client_result.is_error()) fail("failed to create read client");
                    auto client = std::move(client_result.value());

                    for (int r = 0; r < kReadsPerClient; ++r) {
                        const std::string& path = kPaths[static_cast<size_t>(r) % kPaths.size()];
                        auto start = std::chrono::steady_clock::now();
                        auto read_result = client->read(path);
                        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start);
                        if (read_result.is_error() || read_result.value().size() != kFileSize) {
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
            double wall_elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - wall_start).count();

            std::vector<uint64_t> all_latencies;
            uint64_t total_hits = 0;
            uint64_t total_misses = 0;
            for (int t = 0; t < client_count; ++t) {
                auto& thread_samples = per_thread_latencies[static_cast<size_t>(t)];
                all_latencies.insert(all_latencies.end(), thread_samples.begin(), thread_samples.end());
                total_hits += per_thread_hits[static_cast<size_t>(t)];
                total_misses += per_thread_misses[static_cast<size_t>(t)];
            }
            LatencyStats stats = compute_latency_stats(std::move(all_latencies));
            double total_bytes = static_cast<double>(kFileSize) * static_cast<double>(kReadsPerClient) *
                                  static_cast<double>(client_count);
            double hit_ratio = (total_hits + total_misses) > 0
                                    ? static_cast<double>(total_hits) /
                                          static_cast<double>(total_hits + total_misses)
                                    : 0.0;
            double network_disk_bytes = static_cast<double>(total_misses) * static_cast<double>(kFileSize);

            rows.push_back({
                mode.label,
                std::to_string(client_count),
                format_throughput_mb_s(total_bytes, wall_elapsed),
                format_millis(stats.avg_us),
                format_millis(static_cast<double>(stats.p95_us)),
                format_percent(hit_ratio),
                format_mb(network_disk_bytes),
            });
        }
    }

    print_markdown_table(
        "Eksperiment 3 — Cache ON vs OFF",
        {"Cache", "Broj klijenata", "Throughput (MB/s)", "Prosječna latencija", "P95 latencija",
         "Cache hit ratio", "Aproks. network/disk"},
        rows);

    cluster.stop_all();
    return 0;
}
