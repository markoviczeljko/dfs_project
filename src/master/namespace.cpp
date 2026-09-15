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
    pending_blocks_[normalized] = id;
    return Result<BlockId>::ok(id);
}

ErrorCode NamespaceManager::commit_write(const std::string& path, BlockId block_id, uint64_t size) {
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

    // Only accept a block_id this file was actually just given by
    // allocate_block() — otherwise a client could commit an arbitrary,
    // already-written block belonging to a different file (see
    // docs/design.md §11).
    auto pending_it = pending_blocks_.find(normalized);
    if (pending_it == pending_blocks_.end() || pending_it->second != block_id) {
        return ErrorCode::INVALID_REQUEST;
    }
    pending_blocks_.erase(pending_it);

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    // Faza 7 supports exactly one block per file — WRITE overwrites, so the
    // blocks list is replaced rather than appended (see docs/design.md §11).
    it->second.blocks = {block_id};
    it->second.size = size;
    it->second.modification_time = now;
    return ErrorCode::OK;
}
