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

// Real-VM mirror of ../bench_sequential_read.cpp — identical experiment
// definition (same sizes, client counts, repeats, reads-per-client), so
// the two result tables are directly comparable row-for-row in
// docs/benchmark_results_vm.md / the thesis's loopback-vs-VM comparison.
// The only difference is the topology: here the cluster is already running
// on real multipass VMs (see docs/benchmark_results_vm.md's methodology
// header), passed in via --master/--storage instead of being spawned
// locally — this binary never calls spawn_cluster()/stop_all().
namespace {

constexpr std::array<uint64_t, 7> kSizes = {
    1ULL * 1024,        64ULL * 1024,       1ULL * 1024 * 1024, 10ULL * 1024 * 1024,
    BLOCK_SIZE,         2ULL * BLOCK_SIZE,  4ULL * BLOCK_SIZE,
};

constexpr int kRepeats = 3;
constexpr std::array<int, 3> kClientCounts = {1, 5, 10};
constexpr int kReadsPerClient = 20;

std::string size_label(uint64_t bytes) {
    if (bytes >= 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    if (bytes >= 1024) return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace dfs_bench;
    using dfs_bench_remote::parse_remote_args;

    auto cluster = parse_remote_args(argc, argv);
    const std::string& host = cluster.master.host;
    const uint16_t port = cluster.master.port;

    std::vector<TableRow> rows;

    for (uint64_t size : kSizes) {
        std::vector<uint8_t> payload = make_payload(size, /*seed=*/static_cast<uint32_t>(size));
        std::string path = "/vm_seqread_" + std::to_string(size) + ".bin";

        {
            Client::Config setup_config{host, port};
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
                        Client::Config config{host, port};
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
        "Eksperiment 1 (VM) — Sequential READ (3 ponavljanja po ćeliji, min / mean / max)",
        {"Veličina fajla", "Broj klijenata", "Throughput min/mean/max (MB/s)",
         "Latencija min/mean/max", "P95 latencija (pooled)"},
        rows);

    return 0;
}
