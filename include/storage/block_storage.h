#ifndef DFS_STORAGE_BLOCK_STORAGE_H_
#define DFS_STORAGE_BLOCK_STORAGE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "common/errors.h"
#include "common/types.h"

// Persists blocks under <directory>/blocks/<id>.block, checksummed and
// crash-safe (temp file + fsync + rename). Knows nothing about namespaces
// or replication — purely BlockId -> bytes on local disk.
class BlockStorage {
public:
    static Result<std::unique_ptr<BlockStorage>> create(std::filesystem::path directory,
                                                          uint64_t capacity_bytes);

    BlockStorage(const BlockStorage&) = delete;
    BlockStorage& operator=(const BlockStorage&) = delete;

    ErrorCode write_block(BlockId id, const std::vector<uint8_t>& data);
    Result<std::vector<uint8_t>> read_block(BlockId id);
    ErrorCode delete_block(BlockId id);
    bool block_exists(BlockId id) const;

    uint64_t used_space() const;
    uint64_t free_space() const;

private:
    struct LocalBlockInfo {
        uint64_t size;
        uint32_t checksum;
    };

    BlockStorage(std::filesystem::path blocks_dir, uint64_t capacity_bytes);

    ErrorCode recover();
    std::filesystem::path block_path(BlockId id) const;
    std::filesystem::path temp_path(BlockId id) const;

    std::filesystem::path blocks_dir_;
    uint64_t capacity_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<BlockId, LocalBlockInfo> local_blocks_;
    uint64_t used_space_ = 0;
};

#endif  // DFS_STORAGE_BLOCK_STORAGE_H_
