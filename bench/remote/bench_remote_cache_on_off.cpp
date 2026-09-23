#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "client/client.h"
#include "common/config.h"
#include "remote_common.h"

// Real-VM mirror of ../bench_cache_on_off.cpp — see
// bench_remote_sequential_read.cpp's header comment for the shared
// rationale. The cache itself is entirely client-side (see
// src/client/cache.cpp), so this experiment is still meaningful over a
// real network: a real round-trip miss costs more here than on loopback,
// which is exactly the comparison the thesis draws on (cache benefit is
// even more pronounced once the network is real, not simulated).
namespace {

constexpr uint64_t kFileSize = 10ULL * 1024 * 1024;
constexpr int kReadsPerClient = 20;
constexpr std::array<int, 3> kClientCounts = {1, 5, 10};
constexpr int kRepeats = 3;

struct CacheMode {
    std::string label;
    uint64_t cache_max_bytes;
};

}  // namespace

int main(int argc, char** argv) {
    using namespace dfs_bench;
    using dfs_bench_remote::parse_remote_args;

    auto cluster = parse_remote_args(argc, argv);
    const std::string& host = cluster.master.host;
    const uint16_t port = cluster.master.port;

    const std::array<std::string, 2> kPaths = {"/vm_cache_a.bin", "/vm_cache_b.bin"};
    {
        Client::Config setup_config{host, port};
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
            std::vector<uint64_t> pooled_latencies;
            std::vector<double> avg_us_per_rep;
            std::vector<double> throughput_per_rep;
            std::vector<double> hit_ratio_per_rep;
            std::vector<double> network_disk_bytes_per_rep;

            for (int rep = 0; rep < kRepeats; ++rep) {
                std::vector<std::vector<uint64_t>> per_thread_latencies(
                    static_cast<size_t>(client_count));
                std::vector<uint64_t> per_thread_hits(static_cast<size_t>(client_count), 0);
                std::vector<uint64_t> per_thread_misses(static_cast<size_t>(client_count), 0);
                std::vector<std::thread> threads;

                auto wall_start = std::chrono::steady_clock::now();
                for (int t = 0; t < client_count; ++t) {
                    threads.emplace_back([&, t] {
                        Client::Config config{host, port, BLOCK_SIZE, mode.cache_max_bytes};
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

                std::vector<uint64_t> rep_latencies;
                uint64_t total_hits = 0;
                uint64_t total_misses = 0;
                for (int t = 0; t < client_count; ++t) {
                    auto& thread_samples = per_thread_latencies[static_cast<size_t>(t)];
                    rep_latencies.insert(rep_latencies.end(), thread_samples.begin(),
                                          thread_samples.end());
                    total_hits += per_thread_hits[static_cast<size_t>(t)];
                    total_misses += per_thread_misses[static_cast<size_t>(t)];
                }
                LatencyStats rep_stats = compute_latency_stats(rep_latencies);
                double total_bytes = static_cast<double>(kFileSize) *
                                      static_cast<double>(kReadsPerClient) *
                                      static_cast<double>(client_count);
                double hit_ratio = (total_hits + total_misses) > 0
                                        ? static_cast<double>(total_hits) /
                                              static_cast<double>(total_hits + total_misses)
                                        : 0.0;
                double network_disk_bytes =
                    static_cast<double>(total_misses) * static_cast<double>(kFileSize);

                avg_us_per_rep.push_back(rep_stats.avg_us);
                throughput_per_rep.push_back(total_bytes / (1024.0 * 1024.0) / wall_elapsed);
                hit_ratio_per_rep.push_back(hit_ratio);
                network_disk_bytes_per_rep.push_back(network_disk_bytes);
                pooled_latencies.insert(pooled_latencies.end(), rep_latencies.begin(),
                                         rep_latencies.end());
            }

            LatencyStats pooled_stats = compute_latency_stats(pooled_latencies);
            Aggregate latency_agg = aggregate_values(avg_us_per_rep);
            Aggregate throughput_agg = aggregate_values(throughput_per_rep);
            Aggregate hit_ratio_agg = aggregate_values(hit_ratio_per_rep);
            Aggregate network_disk_agg = aggregate_values(network_disk_bytes_per_rep);

            rows.push_back({
                mode.label,
                std::to_string(client_count),
                format_min_mean_max_mb_s(throughput_agg),
                format_min_mean_max_millis(latency_agg),
                format_millis(static_cast<double>(pooled_stats.p95_us)),
                format_percent(hit_ratio_agg.min) + " / " + format_percent(hit_ratio_agg.mean) +
                    " / " + format_percent(hit_ratio_agg.max),
                format_mb(network_disk_agg.mean),
            });
        }
    }

    print_markdown_table(
        "Eksperiment 3 (VM) — Cache ON vs OFF (3 ponavljanja po ćeliji, min / mean / max)",
        {"Cache", "Broj klijenata", "Throughput min/mean/max (MB/s)", "Latencija min/mean/max",
         "P95 latencija (pooled)", "Cache hit ratio min/mean/max", "Aproks. network/disk (mean)"},
        rows);

    return 0;
}
