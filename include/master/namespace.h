#ifndef DFS_MASTER_NAMESPACE_H_
#define DFS_MASTER_NAMESPACE_H_

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
    ErrorCode delete_file(const std::string& path);
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
    ErrorCode commit_write(const std::string& path, const std::vector<BlockId>& block_ids,
                           uint64_t size);

private:
    static Result<std::string> normalize_path(const std::string& path);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, FileMetadata> files_;
    std::unordered_map<std::string, std::unordered_set<BlockId>> pending_blocks_;
    FileId next_file_id_ = 1;
    BlockId next_block_id_ = 1;
};

#endif  // DFS_MASTER_NAMESPACE_H_
