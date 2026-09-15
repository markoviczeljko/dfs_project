#include "master/namespace.h"

#include <chrono>
#include <mutex>
#include <set>
#include <utility>

Result<std::string> NamespaceManager::normalize_path(const std::string& path) {
    if (path.empty() || path.front() != '/') {
        return Result<std::string>::err(ErrorCode::INVALID_REQUEST);
    }

    std::vector<std::string> segments;
    size_t i = 1;
    while (i <= path.size()) {
        size_t next = path.find('/', i);
        std::string segment =
            (next == std::string::npos) ? path.substr(i) : path.substr(i, next - i);

        if (next == std::string::npos) {
            if (!segment.empty()) {
                if (segment == "." || segment == "..") {
                    return Result<std::string>::err(ErrorCode::INVALID_REQUEST);
                }
                segments.push_back(segment);
            }
            break;
        }

        if (segment.empty() || segment == "." || segment == "..") {
            return Result<std::string>::err(ErrorCode::INVALID_REQUEST);
        }
        segments.push_back(segment);
        i = next + 1;
    }

    if (segments.empty()) {
        return Result<std::string>::ok("/");
    }

    std::string normalized;
    for (const auto& segment : segments) {
        normalized += "/" + segment;
    }
    return Result<std::string>::ok(normalized);
}

ErrorCode NamespaceManager::create_file(const std::string& path) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return normalized_result.error();
    }
    const std::string& normalized = normalized_result.value();
    if (normalized == "/") {
        return ErrorCode::INVALID_REQUEST;
    }

    std::unique_lock lock(mutex_);
    if (files_.find(normalized) != files_.end()) {
        return ErrorCode::FILE_ALREADY_EXISTS;
    }

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    FileMetadata metadata{next_file_id_++, normalized, 0, now, {}};
    files_.emplace(normalized, std::move(metadata));
    return ErrorCode::OK;
}

ErrorCode NamespaceManager::delete_file(const std::string& path) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return normalized_result.error();
    }

    std::unique_lock lock(mutex_);
    auto it = files_.find(normalized_result.value());
    if (it == files_.end()) {
        return ErrorCode::FILE_NOT_FOUND;
    }
    files_.erase(it);
    pending_blocks_.erase(normalized_result.value());
    return ErrorCode::OK;
}

Result<FileMetadata> NamespaceManager::stat_file(const std::string& path) const {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return Result<FileMetadata>::err(normalized_result.error());
    }

    std::shared_lock lock(mutex_);
    auto it = files_.find(normalized_result.value());
    if (it == files_.end()) {
        return Result<FileMetadata>::err(ErrorCode::FILE_NOT_FOUND);
    }
    return Result<FileMetadata>::ok(it->second);
}

Result<std::vector<std::string>> NamespaceManager::list_files(const std::string& path) const {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return Result<std::vector<std::string>>::err(normalized_result.error());
    }

    const std::string& normalized = normalized_result.value();
    std::string prefix = (normalized == "/") ? "/" : normalized + "/";

    std::shared_lock lock(mutex_);
    std::set<std::string> children;
    for (const auto& [file_path, metadata] : files_) {
        (void)metadata;
        if (file_path.size() <= prefix.size()) continue;
        if (file_path.compare(0, prefix.size(), prefix) != 0) continue;

        std::string remainder = file_path.substr(prefix.size());
        size_t slash_pos = remainder.find('/');
        std::string child = (slash_pos == std::string::npos) ? remainder : remainder.substr(0, slash_pos);
        children.insert(child);
    }

    return Result<std::vector<std::string>>::ok(
        std::vector<std::string>(children.begin(), children.end()));
}

Result<BlockId> NamespaceManager::allocate_block(const std::string& path) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return Result<BlockId>::err(normalized_result.error());
    }
    const std::string& normalized = normalized_result.value();

    std::unique_lock lock(mutex_);
    if (files_.find(normalized) == files_.end()) {
        return Result<BlockId>::err(ErrorCode::FILE_NOT_FOUND);
    }
    BlockId id = next_block_id_++;
    pending_blocks_[normalized].insert(id);
    return Result<BlockId>::ok(id);
}

ErrorCode NamespaceManager::commit_write(const std::string& path,
                                          const std::vector<BlockId>& block_ids, uint64_t size) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return normalized_result.error();
    }
    const std::string& normalized = normalized_result.value();

    std::unique_lock lock(mutex_);
    auto it = files_.find(normalized);
    if (it == files_.end()) {
        return ErrorCode::FILE_NOT_FOUND;
    }

    // Every id being committed must have been allocated for this path at
    // some point since its last successful commit/delete (subset check
    // against the pending set) — otherwise a client could commit an
    // arbitrary, already-written block belonging to a different path (see
    // docs/design.md §12).
    auto pending_it = pending_blocks_.find(normalized);
    for (BlockId id : block_ids) {
        if (pending_it == pending_blocks_.end() ||
            pending_it->second.find(id) == pending_it->second.end()) {
            return ErrorCode::INVALID_REQUEST;
        }
    }
    // Clear the whole pending set, not just the committed ids — any
    // leftover ids from an earlier abandoned write attempt on this path are
    // discarded here rather than lingering forever (see docs/design.md §12).
    pending_blocks_.erase(normalized);

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    // WRITE overwrites — the blocks list is replaced wholesale, in the
    // caller-given order, which is the file's byte order (see
    // docs/design.md §12).
    it->second.blocks = block_ids;
    it->second.size = size;
    it->second.modification_time = now;

    for (BlockId id : block_ids) {
        auto block_it = blocks_.find(id);
        if (block_it != blocks_.end()) {
            block_it->second.state = BlockState::COMMITTED;
        }
    }
    return ErrorCode::OK;
}

ErrorCode NamespaceManager::set_block_replicas(BlockId block_id,
                                                std::vector<StorageNodeId> replicas) {
    std::unique_lock lock(mutex_);
    blocks_[block_id] = BlockMetadata{block_id, 0, 0, std::move(replicas), BlockState::ALLOCATED};
    return ErrorCode::OK;
}

Result<BlockMetadata> NamespaceManager::block_metadata(BlockId block_id) const {
    std::shared_lock lock(mutex_);
    auto it = blocks_.find(block_id);
    if (it == blocks_.end()) {
        return Result<BlockMetadata>::err(ErrorCode::BLOCK_NOT_FOUND);
    }
    return Result<BlockMetadata>::ok(it->second);
}
