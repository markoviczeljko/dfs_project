#ifndef DFS_MASTER_METADATA_H_
#define DFS_MASTER_METADATA_H_

#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"

struct FileMetadata {
    FileId id;
    std::string path;
    uint64_t size;
    uint64_t modification_time;
    std::vector<BlockId> blocks;
};

// Master-side recovery bookkeeping state (transitions implemented in the
// Failure Handling phase) — defined here now so the type exists wherever
// BlockMetadata is used.
enum class BlockState {
    ALLOCATED,
    WRITING,
    COMMITTED,
    UNDER_REPLICATED,
    RECOVERING,
    CORRUPTED,
    DELETING
};

struct BlockMetadata {
    BlockId id;
    uint64_t size;
    uint64_t version;
    std::vector<StorageNodeId> replicas;
    BlockState state;
};

#endif  // DFS_MASTER_METADATA_H_
