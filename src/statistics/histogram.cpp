#include "statistics/histogram.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace {

size_t bucket_index(uint64_t value) {
    // std::bit_width(0) == 0, std::bit_width(v) == floor(log2(v)) + 1 for
    // v > 0 — see the class comment in histogram.h for the bucket layout.
    size_t index = static_cast<size_t>(std::bit_width(value));
    return std::min(index, Histogram::kNumBuckets - 1);
}

// Inverse of bucket_index: the largest microsecond value that bucket b can
// hold (used as the reported percentile value — a coarse upper-bound
// estimate, not the exact latency).
uint64_t bucket_upper_bound(size_t bucket) {
    if (bucket == 0) return 0;
    if (bucket >= 64) return UINT64_MAX;
    return (uint64_t{1} << bucket) - 1;
}

}  // namespace

void Histogram::record(std::chrono::microseconds latency) {
    int64_t count = latency.count();
    uint64_t value = count > 0 ? static_cast<uint64_t>(count) : 0;
    buckets_[bucket_index(value)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t Histogram::count() const {
    uint64_t total = 0;
    for (const auto& bucket : buckets_) {
        total += bucket.load(std::memory_order_relaxed);
    }
    return total;
}

uint64_t Histogram::percentile(double fraction) const {
    uint64_t total = count();
    if (total == 0) return 0;

    // Smallest rank R such that at least `fraction` of all samples are <=
    // the value at rank R (standard nearest-rank percentile definition),
    // clamped to [1, total] so e.g. a single sample always satisfies any
    // percentile at that sample's own bucket rather than bucket 0.
    double raw_target = fraction * static_cast<double>(total);
    uint64_t target = raw_target <= 1.0 ? 1 : static_cast<uint64_t>(std::ceil(raw_target));
    target = std::min(target, total);

    uint64_t cumulative = 0;
    for (size_t b = 0; b < kNumBuckets; ++b) {
        cumulative += buckets_[b].load(std::memory_order_relaxed);
        if (cumulative >= target) {
            return bucket_upper_bound(b);
        }
    }
    return bucket_upper_bound(kNumBuckets - 1);
}
