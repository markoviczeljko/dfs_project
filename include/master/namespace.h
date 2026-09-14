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

private:
    static Result<std::string> normalize_path(const std::string& path);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, FileMetadata> files_;
    FileId next_file_id_ = 1;
};

#endif  // DFS_MASTER_NAMESPACE_H_
