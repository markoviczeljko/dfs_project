#ifndef DFS_MASTER_NAMESPACE_H_
#define DFS_MASTER_NAMESPACE_H_

#include <condition_variable>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/errors.h"
#include "common/types.h"
#include "master/metadata.h"

// In-memory hierarchical namespace: path -> FileMetadata. Directories are
// implicit path prefixes, not stored entities — see docs/design.md.
class NamespaceManager {
public:
    NamespaceManager() = default;

    NamespaceManager(const NamespaceManager&) = delete;
    NamespaceManager& operator=(const NamespaceManager&) = delete;

    ErrorCode create_file(const std::string& path);

    // Faza 11: returns the blocks the deleted file owned (with their
    // replica snapshots) so Master can dispatch DELETE_BLOCK to physically
    // remove them — see docs/design.md §15. An empty vector (not an error)
    // means the file had zero blocks.
    Result<std::vector<OrphanedBlock>> delete_file(const std::string& path);
    Result<FileMetadata> stat_file(const std::string& path) const;
    Result<std::vector<std::string>> list_files(const std::string& path) const;

    // Faza 7: BlockId issuance and post-write metadata commit. See
    // docs/design.md §11 — no design exists anywhere for how Master should
    // generate BlockIds, so this mirrors next_file_id_'s existing pattern.
    //
    // Faza 8: a multi-block write calls allocate_block() once per chunk
    // before a single final commit_write() covering the whole ordered block
    // list, so every block allocated for a path since its last successful
    // commit/delete must stay simultaneously valid — pending_blocks_ tracks
    // a *set* of outstanding ids per path rather than a single one.
    // commit_write() accepts a block_id only if it is present in that path's
    // pending set (subset check, not exact-match — see docs/design.md §12);
    // this deliberately drops Faza 7's stronger guarantee that an older
    // allocation for a path is invalidated the moment a newer one for the
    // same path is issued, which is incompatible with multi-chunk writes
    // needing several simultaneously-valid pending allocations. Cross-path
    // theft (committing under path A a block_id allocated for path B) stays
    // fully rejected either way, since that id never enters A's pending set.
    // On any successful commit_write(), the path's *entire* pending set is
    // cleared (not just the ids that were committed) — this discards any
    // leftover ids from an earlier abandoned write attempt on the same path,
    // which would otherwise accumulate forever with no way to ever be
    // committed (see docs/design.md §12).
    Result<BlockId> allocate_block(const std::string& path);

    // Faza 11: WRITE overwrites the file's whole block list; the blocks it
    // replaces are returned as orphans (see delete_file()'s comment above
    // and docs/design.md §15) so Master can physically clean them up the
    // same way. Safe to capture unconditionally before the swap: old and
    // new block-id sets can never overlap, since allocate_block() strictly
    // increments next_block_id_.
    Result<std::vector<OrphanedBlock>> commit_write(const std::string& path,
                                                     const std::vector<BlockId>& block_ids,
                                                     uint64_t size, uint64_t last_block_size);

    // Faza 10: resolves how much of a pending append fits into a file's
    // current last block, optimistically reserving that space (bumping
    // BlockMetadata.reserved_size) before any I/O happens — this is what
    // serializes concurrent appends to the same file without holding the
    // lock across the network round-trips that follow (see docs/design.md
    // §14). has_target == false means the caller should treat the whole
    // append as brand-new blocks (empty file, or last block already full).
    Result<AppendTarget> resolve_append_target(const std::string& path, uint64_t append_size);

    // Finalizes an append. When has_existing_target is true, this is a
    // compare-and-swap against the block's *confirmed* size: it only
    // applies once the block's size equals expected_prior_size exactly,
    // waiting (bounded by APPEND_COMMIT_WAIT_TIMEOUT_MS) if an earlier
    // append to the same block hasn't committed yet — otherwise a
    // later-offset commit landing first could move size backwards and hide
    // the earlier append (see docs/design.md §14). new_block_ids go through
    // the same pending-set subset check as commit_write(), but extend
    // file.blocks rather than replacing it.
    // request.last_new_block_size is the actual byte length of the LAST
    // block in request.new_block_ids — needed for the same reason
    // commit_write() takes a last_block_size: a freshly allocated block's
    // reserved_size is never otherwise bumped away from 0, so without this
    // the next append to that block would compute offset 0 and silently
    // overwrite it (see docs/design.md §14). Interior (non-last) new blocks
    // don't need their size recorded — like commit_write()'s blocks, they
    // are never revisited as an append target. existing_block_id, when
    // has_existing_target is set, is verified to be the path's actual
    // current last block before anything is mutated — an unrelated or
    // stale block id is rejected with INVALID_REQUEST rather than silently
    // corrupting a different file's metadata.
    ErrorCode commit_append(const std::string& path, const AppendCommitRequest& request);

    // Faza 9: records which Storage Nodes hold a block, once Master has
    // chosen its replicas (see docs/design.md §13). Creates a new
    // BlockMetadata entry in state ALLOCATED. commit_write() transitions a
    // committed block's entry to COMMITTED. BlockState::WRITING and
    // UNDER_REPLICATED are never set in this phase — Master is never in
    // the data-plane write loop (Client writes directly to Storage Nodes),
    // so there is no observable trigger for WRITING here, and
    // UNDER_REPLICATED is defined as a consequence of heartbeat-driven
    // failure detection, which doesn't exist until Faza 12.
    ErrorCode set_block_replicas(BlockId block_id, std::vector<StorageNodeId> replicas);
    Result<BlockMetadata> block_metadata(BlockId block_id) const;

    // Faza 12.2: recovery support (see docs/design.md §16).
    // All blocks whose replica list currently includes node_id — what
    // RecoveryManager::handle_node_failure() needs to find affected blocks.
    std::vector<BlockId> blocks_with_replica(StorageNodeId node_id) const;
    // Marks a block UNDER_REPLICATED. A no-op (not an error) if block_id is
    // unknown — a block can vanish (DELETE/overwrite) between being found
    // by blocks_with_replica() and being processed.
    void mark_under_replicated(BlockId block_id);
    // One atomic update once a new replica is confirmed valid: replaces
    // old_node with new_node in the block's replica list and sets state
    // (normally COMMITTED, closing the UNDER_REPLICATED window). A no-op
    // if block_id is unknown (same reasoning as mark_under_replicated) or
    // old_node isn't actually in its replica list.
    void update_block_replicas_after_recovery(BlockId block_id, StorageNodeId old_node,
                                               StorageNodeId new_node, BlockState new_state);

private:
    static Result<std::string> normalize_path(const std::string& path);

    mutable std::shared_mutex mutex_;
    mutable std::condition_variable_any block_commit_cv_;
    std::unordered_map<std::string, FileMetadata> files_;
    std::unordered_map<std::string, std::unordered_set<BlockId>> pending_blocks_;
    std::unordered_map<BlockId, BlockMetadata> blocks_;
    FileId next_file_id_ = 1;
    BlockId next_block_id_ = 1;
};

#endif  // DFS_MASTER_NAMESPACE_H_
