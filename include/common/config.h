#ifndef DFS_COMMON_CONFIG_H_
#define DFS_COMMON_CONFIG_H_

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

#endif  // DFS_COMMON_CONFIG_H_
