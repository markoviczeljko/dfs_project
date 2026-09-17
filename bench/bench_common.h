#ifndef DFS_BENCH_BENCH_COMMON_H_
#define DFS_BENCH_BENCH_COMMON_H_

#include <cstdint>
#include <string>
#include <vector>

#include "process_cluster.h"

// Faza 17.3: shared helpers for bench/*.cpp (see docs/design.md §23).
// NOT GoogleTest-based — per IMPLEMENTATION_PLAN.md's Faza 17 section,
// these are measurement tools validated by physical plausibility of
// their output, not pass/fail assertions.
namespace dfs_bench {

// Registers the live cluster so fail() can tear it down before
// terminating. Call once, right after spawn_cluster() succeeds — a bare
// std::_Exit() alone (see fail() below) skips all destructors, so
// without this the spawned dfs_master/dfs_storage processes would leak
// as orphans still bound to the hardcoded bench ports on any post-spawn
// CREATE/WRITE/READ failure, breaking the *next* bench run with a
// confusing "port already in use" error instead of a clean one (found by
// independent cpp-review before this shipped — see docs/design.md §23).
void register_cluster_for_cleanup(dfs_test::SpawnedCluster* cluster);

// Prints an error to stderr, tears down the registered cluster (if any —
// see register_cluster_for_cleanup() above), and terminates immediately
// via std::_Exit (no stack unwinding — safe to call from any of a bench
// executable's worker threads; only the first concurrent caller actually
// runs cleanup, see bench_common.cpp). A bench tool that hits an
// unexpected ErrorCode (spawn/CREATE/WRITE/READ failure) has no
// meaningful partial result to report, unlike a GoogleTest ASSERT that
// only fails one test case.
[[noreturn]] void fail(const std::string& message);

// Deterministic pseudo-random fill, seeded for reproducibility across
// runs. Content only needs to avoid being a degenerate all-zero buffer;
// not used for anything security-sensitive.
std::vector<uint8_t> make_payload(size_t size, uint32_t seed);

// Percentile/average figures for one batch of latency samples, computed
// by sorting exact microsecond samples the caller timed itself around
// each Client call — deliberately NOT read from Client's internal
// Histogram (include/statistics/histogram.h), which only exposes
// coarse bucket-approximated percentiles and has no public way to merge
// samples across the several per-thread Client instances a multi-client
// scenario needs (see docs/design.md §23).
struct LatencyStats {
    uint64_t count = 0;
    double avg_us = 0.0;
    uint64_t p50_us = 0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
};
LatencyStats compute_latency_stats(std::vector<uint64_t> samples_us);

// One row of a results table — already-formatted strings, callers decide
// units/precision per column.
using TableRow = std::vector<std::string>;

// Prints a GitHub-flavored Markdown table to stdout.
// scripts/run_benchmarks.sh (Faza 17.5) captures each bench executable's
// stdout into docs/benchmark_results.md.
void print_markdown_table(const std::string& title, const std::vector<std::string>& headers,
                           const std::vector<TableRow>& rows);

// Formats bytes-transferred-over-seconds as "MB/s" with 2 decimals, or
// "n/a" if seconds <= 0.
std::string format_throughput_mb_s(double bytes, double seconds);

// Formats a microsecond value as "<N.NNN> ms" — shared so every latency
// column across every bench table uses the same precision (found as a
// cosmetic inconsistency by independent cpp-review: some columns used to
// go through std::to_string's default 6-decimal formatting instead).
std::string format_millis(double microseconds);

// Formats a [0,1] fraction as "<N.N>%".
std::string format_percent(double fraction);

// Formats a byte count as "<N.NN> MB" (not a rate — see
// format_throughput_mb_s for that).
std::string format_mb(double bytes);

// Formats a seconds value as "<N.NN> s" — shared for the same reason as
// format_millis (independent cpp-review caught one column that had
// slipped through using std::to_string's raw 6-decimal formatting).
std::string format_seconds(double seconds);

// Extracts the unsigned integer value of `key=<value>` from the line in
// `stats_text` (a dfs_statsctl GET_STATS response, see
// StatisticsManager::format_snapshot()) that contains `line_prefix` —
// scoped to that one line, NOT a global substring search, since several
// keys ("successful=", "bytes=") repeat verbatim across multiple
// sections/per-operation lines (see docs/design.md §20's Faza 16
// test_recovery.cpp bug for why a naive global search silently reads the
// wrong occurrence). Returns 0 if the prefix or key isn't found.
uint64_t parse_stat_u64(const std::string& stats_text, const std::string& line_prefix,
                         const std::string& key);

}  // namespace dfs_bench

#endif  // DFS_BENCH_BENCH_COMMON_H_
