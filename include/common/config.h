#ifndef DFS_COMMON_CONFIG_H_
#define DFS_COMMON_CONFIG_H_

#include <cstdint>

constexpr uint64_t BLOCK_SIZE = 64ULL * 1024 * 1024;
constexpr uint32_t DEFAULT_REPLICATION_FACTOR = 3;
constexpr int HEARTBEAT_INTERVAL_SECONDS = 2;
constexpr int FAILURE_TIMEOUT_SECONDS = 6;

#endif  // DFS_COMMON_CONFIG_H_
