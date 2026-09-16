#ifndef DFS_STORAGE_BLOCK_STORAGE_H_
#define DFS_STORAGE_BLOCK_STORAGE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/errors.h"
#include "common/types.h"

// Persists blocks under <directory>/blocks/<id>.block, checksummed and
// crash-safe (temp file + fsync + rename). Knows nothing about namespaces
// or replication — purely BlockId -> bytes on local disk.
//
// Faza 15: per-block locking (see docs/design.md §19). Each block owns its
// own BlockState{shared_mutex; ...}, so a WRITE to block A never blocks a
// concurrent READ/WRITE of block B. Three lock levels, always acquired one
// at a time, never nested (map_mutex_ for map structure, one BlockState's
// own mutex for its data, space_mutex_ for used_space_ accounting only,
// never held across disk I/O). A BlockState, once created for an id, is
// NEVER erased from local_blocks_ — delete_block() only clears its
// has_data/info under the block's own lock, exactly like a first write
// that got rejected. This is deliberate: erasing would let a concurrent
// write_block() (which already resolved its shared_ptr<BlockState> via
// get_or_create_block_state() before delete_block() ran) finish writing
// into a BlockState no longer reachable from the map — a real orphaned-
// write bug caught in review. Never erasing means get_or_create/find
// always return the same persistent object for a given id, so that race
// can't happen; the cost is a permanent (bounded, one-per-ever-used-id)
// tombstone entry, acceptable for this project's scope.
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
    // read-modify-write done under ONE lock acquisition on this block's own
    // BlockState::mutex, not a separate read_block()+write_block() pair, to
    // avoid a lost-update race between two concurrent appends to the same
    // block (see docs/design.md §19 for the Faza 15 stress test proving
    // this).
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

    // Faza 13: version-check-then-read, atomic under a single lock
    // acquisition on this block's own BlockState::mutex (see docs/design.md
    // §17 — cpp-reviewer finding, still preserved under Faza 15's per-block
    // scheme). not_modified is true (data left empty) when known_version
    // matches the current version; known_version == 0 never matches (real
    // versions start at 1, see docs/design.md §16).
    struct ConditionalReadResult {
        bool not_modified;
        uint64_t version;
        std::vector<uint8_t> data;
    };
    Result<ConditionalReadResult> read_block_if_changed(BlockId id, uint64_t known_version);
    // Snapshot of every block currently on disk — used to build a
    // NODE_INVENTORY report (Faza 12.3) and by RecoveryManager/tests to
    // confirm a node's actual contents without going through read_block()
    // per id. Faza 15: atomic per block, not across the whole snapshot — no
    // known caller needs cross-block atomicity, and holding one lock across
    // every block again would reintroduce the contention this phase removes
    // (see docs/design.md §19).
    std::vector<std::pair<BlockId, BlockInfo>> enumerate_blocks() const;

    uint64_t used_space() const;
    uint64_t free_space() const;

private:
    struct LocalBlockInfo {
        uint64_t size = 0;
        uint32_t checksum = 0;
        uint64_t version = 0;
    };

    // One per block. has_data distinguishes "never successfully written (or
    // deleted)" from "has real data on disk" — a BlockState can exist in
    // local_blocks_ without has_data (e.g. a first write that got rejected
    // with STORAGE_FULL still needs somewhere to park its lock), so map
    // presence alone is deliberately NOT "block exists" — every read path
    // checks has_data too.
    struct BlockState {
        mutable std::shared_mutex mutex;
        bool has_data = false;
        LocalBlockInfo info;
    };

    BlockStorage(std::filesystem::path blocks_dir, uint64_t capacity_bytes);

    ErrorCode recover();
    std::filesystem::path block_path(BlockId id) const;
    std::filesystem::path temp_path(BlockId id) const;

    // Returns nullptr without creating an entry if id isn't known yet.
    std::shared_ptr<BlockState> find_block_state(BlockId id) const;
    // Returns the existing state, or inserts and returns a fresh
    // (has_data == false) one.
    std::shared_ptr<BlockState> get_or_create_block_state(BlockId id);
    // Check-and-reserve for the cross-block used_space_ total; rejects with
    // STORAGE_FULL before any I/O happens. A negative delta always
    // succeeds (releasing space back).
    ErrorCode try_apply_space_delta(int64_t delta);

    // Cores that assume `state.mutex` is already held (unique for write,
    // shared or unique for read) by the caller — factored out so
    // write_block_at() can do its read-modify-write under a single
    // acquisition (see docs/design.md §14).
    ErrorCode write_block_locked(BlockState& state, BlockId id, const std::vector<uint8_t>& data,
                                  uint64_t block_version);
    Result<std::vector<uint8_t>> read_block_locked(const BlockState& state, BlockId id) const;

    std::filesystem::path blocks_dir_;
    uint64_t capacity_;

    mutable std::shared_mutex map_mutex_;
    std::unordered_map<BlockId, std::shared_ptr<BlockState>> local_blocks_;

    mutable std::mutex space_mutex_;
    uint64_t used_space_ = 0;
};

#endif  // DFS_STORAGE_BLOCK_STORAGE_H_
