#ifndef DFS_STATISTICS_HISTOGRAM_H_
#define DFS_STATISTICS_HISTOGRAM_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

// Faza 14: cheap, lock-free latency percentile tracking (see docs/design.md
// §18, design notes §10.9/§10.38/§10.39). The design notes deliberately
// leave the percentile algorithm unspecified beyond "must be cheap, not
// per-request" — this uses fixed power-of-two-microsecond buckets rather
// than reservoir sampling or a sorted sample buffer, so record() is a
// single atomic increment (no locking, no allocation) and percentile()
// only walks fixed-size bucket counts, never raw samples.
//
// Bucket b (for microsecond value v > 0) is std::bit_width(v) — bucket 0
// holds v == 0, bucket b (b >= 1) holds v in [2^(b-1), 2^b - 1]. This is a
// coarse approximation (percentile() returns a bucket's upper bound, not
// the exact latency), which is an accepted tradeoff for O(1) recording.
class Histogram {
public:
    static constexpr size_t kNumBuckets = 65;  // covers the full uint64_t range

    void record(std::chrono::microseconds latency);

    // fraction in [0, 1]. Returns 0 if no samples were recorded yet.
    uint64_t percentile(double fraction) const;

    uint64_t count() const;

private:
    std::array<std::atomic<uint64_t>, kNumBuckets> buckets_{};
};

#endif  // DFS_STATISTICS_HISTOGRAM_H_
