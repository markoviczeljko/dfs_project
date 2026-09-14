#ifndef DFS_STORAGE_BLOCK_MANAGER_H_
#define DFS_STORAGE_BLOCK_MANAGER_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "common/errors.h"
#include "common/types.h"
#include "storage/block_storage.h"

// Thin allocation/lifecycle layer above BlockStorage. allocate_block() is a
// local-only generator for this phase's standalone testing — once networked
// in a later phase, the Master becomes the authority for BlockId issuance,
// and write_block()/read_block() must keep working for ids that never went
// through this class's own allocate_block(). See docs/design.md.
class BlockManager {
public:
    static Result<std::unique_ptr<BlockManager>> create(std::filesystem::path directory,
                                                          uint64_t capacity_bytes);

    BlockManager(const BlockManager&) = delete;
    BlockManager& operator=(const BlockManager&) = delete;

    Result<BlockId> allocate_block();
    ErrorCode release_block(BlockId id);
    ErrorCode write_block(BlockId id, const std::vector<uint8_t>& data);
    Result<std::vector<uint8_t>> read_block(BlockId id);

private:
    explicit BlockManager(std::unique_ptr<BlockStorage> storage);

    std::unique_ptr<BlockStorage> storage_;
    std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::unordered_set<BlockId> allocated_ids_;
};

#endif  // DFS_STORAGE_BLOCK_MANAGER_H_
