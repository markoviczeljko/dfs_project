#ifndef DFS_COMMON_TYPES_H_
#define DFS_COMMON_TYPES_H_

#include <cstdint>

using FileId = uint64_t;
using BlockId = uint64_t;
using StorageNodeId = uint64_t;
// Faza 12.2: idempotency token for WRITE_BLOCK/COPY_BLOCK_DATA retries —
// see storage/operation_log.h and docs/design.md §16.
using OperationId = uint64_t;

#endif  // DFS_COMMON_TYPES_H_
