#include "bench_common.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>

namespace dfs_bench {

namespace {

std::atomic<dfs_test::SpawnedCluster*> g_cluster_to_cleanup{nullptr};
// Guards against two concurrently-failing worker threads both calling
// stop_all() on the same cluster at once, which would race on
// ProcessHandle's non-atomic `reaped_` flag inside wait_for_exit() — only
// the first thread to reach fail() actually runs cleanup; any other
// concurrent caller skips straight to _Exit (harmless: the process is
// terminating either way).
std::atomic_flag g_cleanup_started = ATOMIC_FLAG_INIT;

}  // namespace

void register_cluster_for_cleanup(dfs_test::SpawnedCluster* cluster) {
    g_cluster_to_cleanup.store(cluster, std::memory_order_release);
}

[[noreturn]] void fail(const std::string& message) {
    // Two racing worker threads could interleave partial writes here (no
    // atomicity across the two `<<` calls) — harmless, since the process
    // terminates immediately after either way; not worth a mutex for a
    // benchmark tool's error path.
    std::cerr << "bench: fatal: " << message << "\n";
    if (!g_cleanup_started.test_and_set()) {
        dfs_test::SpawnedCluster* cluster = g_cluster_to_cleanup.load(std::memory_order_acquire);
        if (cluster != nullptr) {
            cluster->stop_all();
        }
    }
    std::_Exit(1);
}

std::vector<uint8_t> make_payload(size_t size, uint32_t seed) {
    std::vector<uint8_t> data(size);
    std::mt19937 rng(seed);
    size_t i = 0;
    while (i + sizeof(uint32_t) <= size) {
        uint32_t word = rng();
        std::memcpy(data.data() + i, &word, sizeof(word));
        i += sizeof(word);
    }
    while (i < size) {
        data[i++] = static_cast<uint8_t>(rng() & 0xFF);
    }
    return data;
}

LatencyStats compute_latency_stats(std::vector<uint64_t> samples_us) {
    LatencyStats stats;
    if (samples_us.empty()) {
        return stats;
    }
    std::sort(samples_us.begin(), samples_us.end());
    stats.count = samples_us.size();
    uint64_t sum = std::accumulate(samples_us.begin(), samples_us.end(), uint64_t{0});
    stats.avg_us = static_cast<double>(sum) / static_cast<double>(stats.count);

    // Lower-nearest-rank: index = floor(fraction * (count-1)). At low
    // sample counts (e.g. count=20) this maps both p95 and p99 to the
    // same index and never selects the single worst sample for any
    // fraction < 1.0 — expected, not a bug (confirmed during
    // Faza 17.3's review). It's exactly why a lone
    // cold-cache-miss outlier among many warm cache-hit reads can pull
    // the mean above p95 for bench_sequential_read.cpp's client_count=1
    // rows — see docs/design.md §23.
    auto at_percentile = [&](double fraction) {
        size_t index = static_cast<size_t>(fraction * static_cast<double>(stats.count - 1));
        return samples_us[index];
    };
    stats.p50_us = at_percentile(0.50);
    stats.p95_us = at_percentile(0.95);
    stats.p99_us = at_percentile(0.99);
    return stats;
}

void print_markdown_table(const std::string& title, const std::vector<std::string>& headers,
                           const std::vector<TableRow>& rows) {
    std::cout << "### " << title << "\n\n";

    std::cout << "|";
    for (const std::string& header : headers) std::cout << " " << header << " |";
    std::cout << "\n|";
    for (size_t i = 0; i < headers.size(); ++i) std::cout << " --- |";
    std::cout << "\n";

    for (const TableRow& row : rows) {
        std::cout << "|";
        for (const std::string& cell : row) std::cout << " " << cell << " |";
        std::cout << "\n";
    }
    std::cout << "\n";
}

std::string format_throughput_mb_s(double bytes, double seconds) {
    if (seconds <= 0.0) {
        return "n/a";
    }
    double mb_per_s = (bytes / (1024.0 * 1024.0)) / seconds;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << mb_per_s;
    return out.str();
}

std::string format_millis(double microseconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << (microseconds / 1000.0) << " ms";
    return out.str();
}

std::string format_percent(double fraction) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << (fraction * 100.0) << "%";
    return out.str();
}

std::string format_mb(double bytes) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << (bytes / (1024.0 * 1024.0)) << " MB";
    return out.str();
}

std::string format_seconds(double seconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << seconds << " s";
    return out.str();
}

Aggregate aggregate_values(const std::vector<double>& values) {
    Aggregate agg;
    if (values.empty()) {
        return agg;
    }
    agg.min = *std::min_element(values.begin(), values.end());
    agg.max = *std::max_element(values.begin(), values.end());
    agg.mean = std::accumulate(values.begin(), values.end(), 0.0) /
               static_cast<double>(values.size());
    return agg;
}

std::string format_min_mean_max_millis(const Aggregate& microseconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << (microseconds.min / 1000.0) << " / "
        << (microseconds.mean / 1000.0) << " / " << (microseconds.max / 1000.0) << " ms";
    return out.str();
}

std::string format_min_mean_max_mb_s(const Aggregate& mb_s) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << mb_s.min << " / " << mb_s.mean << " / " << mb_s.max;
    return out.str();
}

std::string format_min_mean_max_seconds(const Aggregate& seconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << seconds.min << " / " << seconds.mean << " / "
        << seconds.max << " s";
    return out.str();
}

uint64_t parse_stat_u64(const std::string& stats_text, const std::string& line_prefix,
                         const std::string& key) {
    size_t line_start = stats_text.find(line_prefix);
    if (line_start == std::string::npos) {
        return 0;
    }
    size_t line_end = stats_text.find('\n', line_start);
    std::string line = stats_text.substr(
        line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);

    size_t key_pos = line.find(key);
    if (key_pos == std::string::npos) {
        return 0;
    }
    size_t value_start = key_pos + key.size();
    size_t value_end = value_start;
    while (value_end < line.size() && std::isdigit(static_cast<unsigned char>(line[value_end]))) {
        ++value_end;
    }
    if (value_end == value_start) {
        return 0;
    }
    return std::stoull(line.substr(value_start, value_end - value_start));
}

}  // namespace dfs_bench
