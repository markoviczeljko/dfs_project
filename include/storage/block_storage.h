#ifndef DFS_STORAGE_BLOCK_STORAGE_H_
#define DFS_STORAGE_BLOCK_STORAGE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
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

    // block_version defaults to 0 for every pre-Faza-12.2 caller (never
    // meaningfully read before now); Faza 12.2 callers (WRITE_BLOCK's
    // handler, COPY_BLOCK_DATA's handler) pass the real value through from
    // the wire (see docs/design.md §16).
    ErrorCode write_block(BlockId id, const std::vector<uint8_t>& data, uint64_t block_version = 0);
    Result<std::vector<uint8_t>> read_block(BlockId id);

    // Faza 10: writes data at a byte offset into an EXISTING block, used for
    // append-at-offset (see docs/design.md §14). Extends the block if
    // offset+data.size() is past its current length, zero-filling any gap
    // (a concurrent append can legitimately claim a later offset before an
    // earlier one's bytes physically arrive — see docs/design.md §14).
    // Rejects offset+data.size() > BLOCK_SIZE. Goes through the same
    // crash-safe temp-file+fsync+rename path as write_block() — this is a
    // read-modify-write done under ONE lock acquisition, not a separate
    // read_block()+write_block() pair, to avoid a lost-update race between
    // two concurrent appends to the same block.
    ErrorCode write_block_at(BlockId id, uint64_t offset, const std::vector<uint8_t>& data,
                              uint64_t block_version = 0);

    ErrorCode delete_block(BlockId id);
    bool block_exists(BlockId id) const;

    // Faza 12.2: local view of one block's metadata — checksum and version
    // for NODE_INVENTORY reporting (Faza 12.3), not the data itself (see
    // docs/design.md §16).
    struct BlockInfo {
        uint64_t size;
        uint32_t checksum;
        uint64_t version;
    };
    Result<BlockInfo> get_block_info(BlockId id) const;
    // Snapshot of every block currently on disk — used to build a
    // NODE_INVENTORY report (Faza 12.3) and by RecoveryManager/tests to
    // confirm a node's actual contents without going through read_block()
    // per id.
    std::vector<std::pair<BlockId, BlockInfo>> enumerate_blocks() const;

    uint64_t used_space() const;
    uint64_t free_space() const;

private:
    struct LocalBlockInfo {
        uint64_t size;
        uint32_t checksum;
        uint64_t version;
    };

    BlockStorage(std::filesystem::path blocks_dir, uint64_t capacity_bytes);

    ErrorCode recover();
    std::filesystem::path block_path(BlockId id) const;
    std::filesystem::path temp_path(BlockId id) const;

    // Faza 10: lock-free cores of write_block()/read_block(), factored out
    // so write_block_at() can do its read-modify-write under a single
    // unique_lock (see docs/design.md §14).
    ErrorCode write_block_locked(BlockId id, const std::vector<uint8_t>& data, uint64_t block_version);
    Result<std::vector<uint8_t>> read_block_locked(BlockId id) const;

    std::filesystem::path blocks_dir_;
    uint64_t capacity_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<BlockId, LocalBlockInfo> local_blocks_;
    uint64_t used_space_ = 0;
};

#endif  // DFS_STORAGE_BLOCK_STORAGE_H_
