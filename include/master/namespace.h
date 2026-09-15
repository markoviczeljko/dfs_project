#ifndef DFS_MASTER_NAMESPACE_H_
#define DFS_MASTER_NAMESPACE_H_

#include <shared_mutex>
#include <string>
#include <unordered_map>
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
    // commit_write() only accepts a block_id this file was most recently
    // given by allocate_block() — otherwise a client could point a file at
    // an unrelated, already-written block belonging to someone else.
    Result<BlockId> allocate_block(const std::string& path);
    ErrorCode commit_write(const std::string& path, BlockId block_id, uint64_t size);

private:
    static Result<std::string> normalize_path(const std::string& path);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, FileMetadata> files_;
    std::unordered_map<std::string, BlockId> pending_blocks_;
    FileId next_file_id_ = 1;
    BlockId next_block_id_ = 1;
};

#endif  // DFS_MASTER_NAMESPACE_H_
