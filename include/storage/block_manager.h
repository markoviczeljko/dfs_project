#ifndef DFS_STORAGE_BLOCK_MANAGER_H_
#define DFS_STORAGE_BLOCK_MANAGER_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>
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
    ErrorCode write_block(BlockId id, const std::vector<uint8_t>& data, uint64_t block_version = 0);
    Result<std::vector<uint8_t>> read_block(BlockId id);
    ErrorCode write_block_at(BlockId id, uint64_t offset, const std::vector<uint8_t>& data,
                              uint64_t block_version = 0);

    // Faza 12.2: thin passthroughs to BlockStorage — see its own comments
    // for what these expose (docs/design.md §16).
    Result<BlockStorage::BlockInfo> get_block_info(BlockId id) const;
    std::vector<std::pair<BlockId, BlockStorage::BlockInfo>> enumerate_blocks() const;

    // Faza 13: see BlockStorage::read_block_if_changed.
    Result<BlockStorage::ConditionalReadResult> read_block_if_changed(BlockId id, uint64_t known_version);

    // Faza 14: capacity stats passthroughs (see docs/design.md §18).
    uint64_t used_space() const;
    uint64_t free_space() const;

private:
    explicit BlockManager(std::unique_ptr<BlockStorage> storage);

    std::unique_ptr<BlockStorage> storage_;
    std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::unordered_set<BlockId> allocated_ids_;
};

#endif  // DFS_STORAGE_BLOCK_MANAGER_H_
