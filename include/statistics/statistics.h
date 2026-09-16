#ifndef DFS_STATISTICS_STATISTICS_H_
#define DFS_STATISTICS_STATISTICS_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "common/errors.h"
#include "statistics/histogram.h"

// Faza 14: centralized statistics collection (see docs/design.md §18,
// design notes §10). Reconciles two conflicting sketches in the design
// notes (§10.28-§10.30's camelCase StatisticsManager/Statistics split vs
// §17.13's snake_case Statistics) by following this project's own
// established convention instead of either literal sketch: snake_case
// throughout, one class (no separate raw-counter struct exposed
// publicly), parameterized record_*() calls since bytes/latency are
// needed (see docs/design.md §18 for the full reasoning).
//
// Deliberately out of scope (documented, not silently skipped — see
// docs/design.md §18): per-MessageType network byte/message counters
// (§10.14, would need a hook in every TCPConnection call site project-wide
// for a stat the plan's own committed method list never named), and
// request/operation-ID correlation (§10.25-§10.26, a tracing concern, not
// a countable metric).
enum class OperationType { READ, WRITE, APPEND, CREATE, DELETE, LIST, STAT };
constexpr size_t kNumOperationTypes = 7;

struct OperationStats {
    uint64_t total = 0;
    uint64_t successful = 0;
    uint64_t failed = 0;
    uint64_t bytes = 0;
    uint64_t p50_us = 0;
    uint64_t p90_us = 0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
};

struct StatisticsSnapshot {
    std::array<OperationStats, kNumOperationTypes> operations{};
    std::array<uint64_t, 11> errors_by_code{};  // indexed by ErrorCode's underlying value

    uint64_t replication_operations = 0;
    uint64_t bytes_replicated = 0;

    uint64_t recovery_operations = 0;
    uint64_t successful_recoveries = 0;
    uint64_t bytes_recovered = 0;

    uint64_t detected_failures = 0;

    uint64_t heartbeats_sent = 0;
    uint64_t heartbeat_send_failures = 0;
    uint64_t heartbeats_received = 0;

    uint64_t storage_capacity_bytes = 0;
    uint64_t storage_used_bytes = 0;
    uint64_t storage_block_count = 0;
};

class StatisticsManager {
public:
    void record_operation(OperationType type, uint64_t bytes, std::chrono::microseconds latency,
                           bool success);
    void record_read(uint64_t bytes, std::chrono::microseconds latency, bool success);
    void record_write(uint64_t bytes, std::chrono::microseconds latency, bool success);
    void record_append(uint64_t bytes, std::chrono::microseconds latency, bool success);

    void record_error(ErrorCode code);

    // Master-side: a freshly-committed block's initial RF-wide placement
    // (see docs/design.md §18 — distinct from record_recovery, which is
    // failure-driven re-replication, not initial placement).
    void record_replication(uint64_t bytes);

    // Master-side (RecoveryManager): one COPY_BLOCK-driven re-replication
    // attempt, successful or not.
    void record_recovery(uint64_t bytes, bool success);

    void record_node_dead();

    void record_heartbeat_sent(bool success);
    void record_heartbeat_received();

    // Gauge, not an accumulator — overwrites the previous value each call
    // (StorageNode capacity doesn't "add up" across calls).
    void record_storage_capacity(uint64_t capacity_bytes, uint64_t used_bytes,
                                  uint64_t block_count);

    StatisticsSnapshot snapshot() const;

    // Human-readable "DFS Statistics" table (design notes §10.33's
    // dfsctl-stats example) — this is GET_STATS's wire payload verbatim,
    // so dfs_statsctl only needs to print bytes back, no client-side
    // parsing/formatting.
    std::string format_snapshot() const;

private:
    struct OperationCounters {
        std::atomic<uint64_t> total{0};
        std::atomic<uint64_t> successful{0};
        std::atomic<uint64_t> failed{0};
        std::atomic<uint64_t> bytes{0};
        Histogram latency;
    };

    std::array<OperationCounters, kNumOperationTypes> operations_;
    std::array<std::atomic<uint64_t>, 11> errors_by_code_{};

    std::atomic<uint64_t> replication_operations_{0};
    std::atomic<uint64_t> bytes_replicated_{0};

    std::atomic<uint64_t> recovery_operations_{0};
    std::atomic<uint64_t> successful_recoveries_{0};
    std::atomic<uint64_t> bytes_recovered_{0};

    std::atomic<uint64_t> detected_failures_{0};

    std::atomic<uint64_t> heartbeats_sent_{0};
    std::atomic<uint64_t> heartbeat_send_failures_{0};
    std::atomic<uint64_t> heartbeats_received_{0};

    // Gauges, protected by a mutex rather than 3 independent atomics —
    // capacity/used/block_count are only ever meaningful read together as
    // one consistent triple (see docs/design.md §18).
    mutable std::mutex capacity_mutex_;
    uint64_t storage_capacity_bytes_ = 0;
    uint64_t storage_used_bytes_ = 0;
    uint64_t storage_block_count_ = 0;
};

#endif  // DFS_STATISTICS_STATISTICS_H_
