#ifndef DFS_COMMON_CONFIG_H_
#define DFS_COMMON_CONFIG_H_

#include <cstddef>
#include <cstdint>

constexpr uint64_t BLOCK_SIZE = 64ULL * 1024 * 1024;
constexpr uint32_t DEFAULT_REPLICATION_FACTOR = 3;
constexpr int HEARTBEAT_INTERVAL_SECONDS = 2;
constexpr int FAILURE_TIMEOUT_SECONDS = 6;
constexpr int APPEND_COMMIT_WAIT_TIMEOUT_MS = 3000;

// Faza 11: bounds Master's outbound DELETE_BLOCK dispatch (see
// docs/design.md §15) — a Storage Node that accepts the connection but
// never replies would otherwise block the worker thread handling the
// client's DELETE_FILE/WRITE_COMMIT forever, since Socket has no default
// I/O timeout.
constexpr int DELETE_BLOCK_TIMEOUT_MS = 2000;

// Faza 12.1: node liveness state machine (see docs/design.md §16).
// HEARTBEAT_INTERVAL_SECONDS/FAILURE_TIMEOUT_SECONDS above were already
// reserved since Faza 9 — FAILURE_TIMEOUT_SECONDS becomes the
// HEALTHY->SUSPECTED threshold, and DEAD_TIMEOUT_SECONDS (double it) the
// SUSPECTED->DEAD threshold, so a node gets one full extra failure-timeout
// window as a "maybe just slow" grace period before being written off.
constexpr int SUSPECTED_TIMEOUT_SECONDS = FAILURE_TIMEOUT_SECONDS;
constexpr int DEAD_TIMEOUT_SECONDS = FAILURE_TIMEOUT_SECONDS * 2;
// How often FailureDetector's background thread re-checks every node's
// last-heartbeat age. Independent of the interval a Storage Node actually
// sends heartbeats at — just needs to be comfortably finer-grained than
// SUSPECTED_TIMEOUT_SECONDS so a transition isn't detected many seconds
// late.
constexpr int FAILURE_SWEEP_INTERVAL_MS = 500;
// Bounds a single outbound HEARTBEAT send/recv on the Storage Node side —
// same rationale as DELETE_BLOCK_TIMEOUT_MS (Faza 11): a Master that
// accepts the connection but never replies must not hang the heartbeat
// thread forever.
constexpr int HEARTBEAT_SEND_TIMEOUT_MS = 1000;
// Bugfix: bounds StorageNode's outbound REGISTER_NODE and NODE_INVENTORY
// calls to Master — same rationale as HEARTBEAT_SEND_TIMEOUT_MS/
// COPY_BLOCK_TIMEOUT_MS, missed on these two call sites when they were
// first written. A Master that accepts the connection but never replies
// must not hang StorageNode::start()/heartbeat_loop() forever.
constexpr int MASTER_RPC_TIMEOUT_MS = 5000;

// Faza 12.2: recovery / re-replication + idempotency (see docs/design.md
// §16).
constexpr int MAX_CONCURRENT_RECOVERY_OPERATIONS = 4;
constexpr int MAX_RECOVERY_ATTEMPTS_PER_BLOCK = 5;
// Bounds both legs of a COPY_BLOCK transfer (Master->source, source->
// destination) — same rationale as DELETE_BLOCK_TIMEOUT_MS (Faza 11): a
// peer that accepts the connection but never replies must not hang the
// caller's thread forever.
constexpr int COPY_BLOCK_TIMEOUT_MS = 5000;
// Bounded FIFO — an OperationLog only needs to remember recently-retried
// operations long enough for a client's own bounded retry window to give
// up, not forever.
constexpr size_t OPERATION_LOG_CAPACITY = 1024;

#endif  // DFS_COMMON_CONFIG_H_
