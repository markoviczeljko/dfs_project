#ifndef DFS_STORAGE_OPERATION_LOG_H_
#define DFS_STORAGE_OPERATION_LOG_H_

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/errors.h"
#include "common/types.h"

enum class OperationState { IN_PROGRESS, COMPLETED, FAILED };

struct OperationRecord {
    OperationId id;
    OperationState state;
    ErrorCode result;
    // Cached reply bytes, replayed verbatim on a retry rather than
    // re-executing the operation — see docs/design.md §16.
    std::vector<std::byte> response_payload;
};

// Faza 12.2: per-StorageNode idempotency ledger for mutating operations
// that can be safely retried after a lost ACK (WRITE_BLOCK, COPY_BLOCK_DATA
// — see docs/design.md §16, §8.13/§8.18-§8.20). A bounded FIFO, not an
// ever-growing map: an operation only needs to be remembered long enough
// for the caller's own bounded retry window to give up, not forever.
//
// Shared by WRITE_BLOCK and COPY_BLOCK_DATA rather than one log per
// message type — OperationIds are random 64-bit values (see
// RecoveryManager/Client's next_operation_id()), so cross-namespace
// collision between the two is astronomically unlikely, and a single log
// is simpler than coordinating eviction across two.
class OperationLog {
public:
    explicit OperationLog(size_t capacity = OPERATION_LOG_CAPACITY);

    OperationLog(const OperationLog&) = delete;
    OperationLog& operator=(const OperationLog&) = delete;

    // Returns the remembered record if id has already been seen
    // (IN_PROGRESS: a concurrent/earlier attempt is still running;
    // COMPLETED/FAILED: replay its result) — the caller must not execute
    // again in either case. Returns nullopt and marks id IN_PROGRESS if
    // this is genuinely new — the caller must then execute and call
    // complete().
    std::optional<OperationRecord> begin_or_get(OperationId id);
    void complete(OperationId id, ErrorCode result, std::vector<std::byte> response_payload);

    // Faza 14: read-only peek, never mutates state (unlike begin_or_get()) —
    // used by StorageNode's stats wrapper to detect an idempotent replay
    // BEFORE the handler runs, so a retried operation doesn't get double-
    // counted in StatisticsManager (see docs/design.md §18).
    bool contains(OperationId id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<OperationId, OperationRecord> records_;
    std::deque<OperationId> insertion_order_;
    size_t capacity_;
};

#endif  // DFS_STORAGE_OPERATION_LOG_H_
