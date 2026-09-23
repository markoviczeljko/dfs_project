#include "master/namespace.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <utility>

#include "common/config.h"

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

Result<std::vector<OrphanedBlock>> NamespaceManager::delete_file(const std::string& path) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return Result<std::vector<OrphanedBlock>>::err(normalized_result.error());
    }

    std::unique_lock lock(mutex_);
    auto it = files_.find(normalized_result.value());
    if (it == files_.end()) {
        return Result<std::vector<OrphanedBlock>>::err(ErrorCode::FILE_NOT_FOUND);
    }

    // Snapshot each owned block's replicas before erasing it from blocks_ —
    // Master needs the replica list afterward to dispatch DELETE_BLOCK, and
    // once erased here there's no other way to recover it (see
    // docs/design.md §15). A file.blocks id with no blocks_ entry would be
    // an invariant violation that should never happen; deliberately a
    // no-op here rather than an error, matching the same
    // "if (block_it != blocks_.end())" no-op treatment of that identical
    // condition a few lines below in commit_write()'s COMMITTED-state loop.
    std::vector<OrphanedBlock> orphans;
    for (BlockId id : it->second.blocks) {
        auto block_it = blocks_.find(id);
        if (block_it != blocks_.end()) {
            orphans.push_back(OrphanedBlock{id, block_it->second.replicas});
            blocks_.erase(block_it);
        }
    }

    files_.erase(it);
    pending_blocks_.erase(normalized_result.value());
    return Result<std::vector<OrphanedBlock>>::ok(std::move(orphans));
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

Result<std::vector<OrphanedBlock>> NamespaceManager::abandon_blocks(
    const std::string& path, const std::vector<BlockId>& block_ids) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return Result<std::vector<OrphanedBlock>>::err(normalized_result.error());
    }
    const std::string& normalized = normalized_result.value();

    std::unique_lock lock(mutex_);
    std::vector<OrphanedBlock> orphans;
    auto pending_it = pending_blocks_.find(normalized);
    if (pending_it == pending_blocks_.end()) {
        return Result<std::vector<OrphanedBlock>>::ok(std::move(orphans));
    }
    for (BlockId id : block_ids) {
        if (pending_it->second.erase(id) == 0) {
            continue;  // already committed or dropped elsewhere — idempotent skip
        }
        // No blocks_ entry means ALLOCATE_BLOCK failed before replicas were
        // chosen: nothing was ever sent to a Storage Node, nothing to GC.
        auto block_it = blocks_.find(id);
        if (block_it != blocks_.end()) {
            orphans.push_back(OrphanedBlock{id, block_it->second.replicas});
            blocks_.erase(block_it);
        }
    }
    if (pending_it->second.empty()) {
        pending_blocks_.erase(pending_it);
    }
    return Result<std::vector<OrphanedBlock>>::ok(std::move(orphans));
}

Result<std::vector<OrphanedBlock>> NamespaceManager::commit_write(
    const std::string& path, const std::vector<BlockId>& block_ids, uint64_t size,
    uint64_t last_block_size) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return Result<std::vector<OrphanedBlock>>::err(normalized_result.error());
    }
    const std::string& normalized = normalized_result.value();

    std::unique_lock lock(mutex_);
    auto it = files_.find(normalized);
    if (it == files_.end()) {
        return Result<std::vector<OrphanedBlock>>::err(ErrorCode::FILE_NOT_FOUND);
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
            return Result<std::vector<OrphanedBlock>>::err(ErrorCode::INVALID_REQUEST);
        }
    }
    // Clear the whole pending set, not just the committed ids. Leftover ids
    // from an earlier failed attempt on this path can never be committed
    // after this, so (Faza 18) they become orphans for GC right here rather
    // than keeping their blocks_ entries and on-disk replicas forever (see
    // docs/design.md §12).
    std::vector<OrphanedBlock> orphans;
    if (pending_it != pending_blocks_.end()) {
        const std::unordered_set<BlockId> committing(block_ids.begin(), block_ids.end());
        for (BlockId id : pending_it->second) {
            if (committing.count(id) != 0) {
                continue;
            }
            auto leftover_it = blocks_.find(id);
            if (leftover_it != blocks_.end()) {
                orphans.push_back(OrphanedBlock{id, leftover_it->second.replicas});
                blocks_.erase(leftover_it);
            }
        }
        pending_blocks_.erase(pending_it);
    }

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    // Faza 11: the blocks this WRITE is about to replace become orphans —
    // captured (with their replicas) and erased from blocks_ before the
    // swap below, so Master can physically clean them up afterward. Safe
    // unconditionally: block_ids can never contain any of these, since
    // allocate_block() strictly increments next_block_id_ (see
    // docs/design.md §15). A missing blocks_ entry is a no-op here, same
    // reasoning as delete_file() above.
    for (BlockId id : it->second.blocks) {
        auto old_block_it = blocks_.find(id);
        if (old_block_it != blocks_.end()) {
            orphans.push_back(OrphanedBlock{id, old_block_it->second.replicas});
            blocks_.erase(old_block_it);
        }
    }

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
    // Only the last block is ever an append target, so only its size needs
    // to be accurate — interior blocks are never revisited (see
    // docs/design.md §14).
    if (!block_ids.empty()) {
        auto last_it = blocks_.find(block_ids.back());
        if (last_it != blocks_.end()) {
            last_it->second.size = last_block_size;
            last_it->second.reserved_size = last_block_size;
        }
    }
    return Result<std::vector<OrphanedBlock>>::ok(std::move(orphans));
}

ErrorCode NamespaceManager::set_block_replicas(BlockId block_id,
                                                std::vector<StorageNodeId> replicas) {
    std::unique_lock lock(mutex_);
    // Faza 12.3: a freshly allocated block starts at version 1 — matches
    // what ALLOCATE_BLOCK's response now tells the Client, and what
    // Master::handle_node_inventory() compares an incoming replica report
    // against (see docs/design.md §16).
    blocks_[block_id] = BlockMetadata{block_id, /*size=*/0, /*reserved_size=*/0, /*version=*/1,
                                       std::move(replicas), BlockState::ALLOCATED};
    return ErrorCode::OK;
}

std::vector<BlockId> NamespaceManager::blocks_with_replica(StorageNodeId node_id) const {
    std::shared_lock lock(mutex_);
    std::vector<BlockId> affected;
    for (const auto& [id, metadata] : blocks_) {
        if (std::find(metadata.replicas.begin(), metadata.replicas.end(), node_id) !=
            metadata.replicas.end()) {
            affected.push_back(id);
        }
    }
    return affected;
}

void NamespaceManager::mark_under_replicated(BlockId block_id) {
    std::unique_lock lock(mutex_);
    auto it = blocks_.find(block_id);
    if (it != blocks_.end()) {
        it->second.state = BlockState::UNDER_REPLICATED;
    }
}

void NamespaceManager::update_block_replicas_after_recovery(BlockId block_id, StorageNodeId old_node,
                                                              StorageNodeId new_node,
                                                              BlockState new_state) {
    std::unique_lock lock(mutex_);
    auto it = blocks_.find(block_id);
    if (it == blocks_.end()) {
        return;
    }
    auto& replicas = it->second.replicas;
    auto pos = std::find(replicas.begin(), replicas.end(), old_node);
    if (pos == replicas.end()) {
        return;
    }
    *pos = new_node;
    it->second.state = new_state;
}

void NamespaceManager::remove_replica(BlockId block_id, StorageNodeId node_id) {
    std::unique_lock lock(mutex_);
    auto it = blocks_.find(block_id);
    if (it == blocks_.end()) {
        return;
    }
    auto& replicas = it->second.replicas;
    auto pos = std::find(replicas.begin(), replicas.end(), node_id);
    if (pos != replicas.end()) {
        replicas.erase(pos);
    }
}

Result<AppendTarget> NamespaceManager::resolve_append_target(const std::string& path,
                                                               uint64_t append_size) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return Result<AppendTarget>::err(normalized_result.error());
    }

    std::unique_lock lock(mutex_);
    auto file_it = files_.find(normalized_result.value());
    if (file_it == files_.end()) {
        return Result<AppendTarget>::err(ErrorCode::FILE_NOT_FOUND);
    }

    if (file_it->second.blocks.empty() || append_size == 0) {
        return Result<AppendTarget>::ok(AppendTarget{false, 0, 0, 0, {}, 0});
    }

    BlockId last_id = file_it->second.blocks.back();
    auto block_it = blocks_.find(last_id);
    if (block_it == blocks_.end()) {
        return Result<AppendTarget>::err(ErrorCode::INTERNAL_ERROR);
    }

    if (block_it->second.reserved_size >= BLOCK_SIZE) {
        return Result<AppendTarget>::ok(AppendTarget{false, 0, 0, 0, {}, 0});
    }

    uint64_t free_space = BLOCK_SIZE - block_it->second.reserved_size;
    uint32_t fill = static_cast<uint32_t>(std::min<uint64_t>(append_size, free_space));
    uint64_t offset = block_it->second.reserved_size;
    block_it->second.reserved_size += fill;  // Optimistic reservation, no I/O yet.

    // Faza 12.3: the version this block will carry once the fill commits —
    // commit_append() stamps it on a successful CAS (see docs/design.md
    // §16).
    return Result<AppendTarget>::ok(AppendTarget{true, last_id, offset, fill,
                                                  block_it->second.replicas,
                                                  block_it->second.version + 1});
}

namespace {
// True iff existing_block_id genuinely belongs to path (appears anywhere in
// its block list) — checked instead of "is exactly the current last block"
// because two concurrent appends can legitimately be in flight at once:
// one still committing a fill into a block that, by the time it commits,
// has already been superseded as the last block by another append's
// rollover to a brand-new block (see docs/design.md §14). This still fully
// rejects a block belonging to a DIFFERENT file, which is the actual
// cross-file-corruption case this check exists to prevent. Must be
// re-checked with a FRESH files_ lookup both before and after any wait on
// block_commit_cv_ — condition_variable_any::wait_for() unlocks mutex_
// while waiting, during which a concurrent delete_file() can invalidate an
// iterator taken beforehand.
bool file_owns_block(const FileMetadata& file, bool has_existing_target, BlockId existing_block_id) {
    if (!has_existing_target) return true;
    return std::find(file.blocks.begin(), file.blocks.end(), existing_block_id) != file.blocks.end();
}
}  // namespace

ErrorCode NamespaceManager::commit_append(const std::string& path,
                                          const AppendCommitRequest& request) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return normalized_result.error();
    }
    const std::string& normalized = normalized_result.value();

    std::unique_lock lock(mutex_);

    // Fail fast on an ownership mismatch before waiting on anything —
    // existing_block_id must genuinely be this path's last block, not some
    // unrelated (or stale, or adversarial) block id: without this check a
    // malformed APPEND_COMMIT could mutate a completely different file's
    // block metadata as long as its expected_prior_size happened to match
    // (trivially true for any never-yet-appended block, which starts at
    // size 0) — see docs/design.md §14.
    {
        auto file_it = files_.find(normalized);
        if (file_it == files_.end()) {
            return ErrorCode::FILE_NOT_FOUND;
        }
        if (!file_owns_block(file_it->second, request.has_existing_target,
                                    request.existing_block_id)) {
            return ErrorCode::INVALID_REQUEST;
        }
    }

    if (request.has_existing_target) {
        // Compare-and-swap against the CONFIRMED size: an earlier append to
        // this same block may still be in flight (its reservation already
        // landed, but its commit hasn't arrived yet). Waiting here — rather
        // than rejecting immediately — absorbs ordinary network/scheduling
        // reordering between two concurrent appends without making either
        // one a hard failure (see docs/design.md §14). The predicate does
        // its own fresh blocks_.find() on every check — it's invoked
        // repeatedly under the lock, so no iterator is held across the
        // unlock/relock wait_for() does internally.
        bool ready = block_commit_cv_.wait_for(
            lock, std::chrono::milliseconds(APPEND_COMMIT_WAIT_TIMEOUT_MS), [&] {
                auto it = blocks_.find(request.existing_block_id);
                return it != blocks_.end() && it->second.size == request.expected_prior_size;
            });
        if (!ready) {
            return ErrorCode::TIMEOUT;
        }
    }

    // Re-validate everything from scratch: the wait above (if it happened)
    // released mutex_, so files_/pending_blocks_/blocks_ may have changed
    // underneath this call. No iterator from before the wait is reused
    // below.
    auto file_it = files_.find(normalized);
    if (file_it == files_.end()) {
        return ErrorCode::FILE_NOT_FOUND;
    }
    if (!file_owns_block(file_it->second, request.has_existing_target,
                                request.existing_block_id)) {
        return ErrorCode::INVALID_REQUEST;
    }

    // New blocks go through the same pending-set subset check as
    // commit_write() (see docs/design.md §12), but — unlike commit_write()
    // — only the ids actually being committed are removed, not the whole
    // set: two concurrent appends to the same empty/full-last-block file
    // can each have a simultaneously-valid pending new block at once, and
    // clearing the entire set on the first commit would wrongly reject the
    // second (see docs/design.md §14). Validated before any mutation below,
    // so a bad new_block_ids list can't leave a partially-applied commit
    // (fill applied, new blocks rejected) behind.
    auto pending_it = pending_blocks_.find(normalized);
    for (BlockId id : request.new_block_ids) {
        if (pending_it == pending_blocks_.end() ||
            pending_it->second.find(id) == pending_it->second.end()) {
            return ErrorCode::INVALID_REQUEST;
        }
    }

    // All validation passed — mutate. existing_block_id is looked up fresh
    // one more time rather than reusing any iterator from above.
    if (request.has_existing_target) {
        auto block_it = blocks_.find(request.existing_block_id);
        if (block_it == blocks_.end()) {
            return ErrorCode::INTERNAL_ERROR;
        }
        block_it->second.size = request.expected_prior_size + request.filled_amount;
        block_it->second.state = BlockState::COMMITTED;
        // Faza 12.3: advances the block's version on every successful
        // append commit — lets NODE_INVENTORY reconciliation tell a stale
        // replica apart from a current one (see docs/design.md §16).
        block_it->second.version = request.expected_target_version;
    }

    if (pending_it != pending_blocks_.end()) {
        for (BlockId id : request.new_block_ids) {
            pending_it->second.erase(id);
        }
        if (pending_it->second.empty()) {
            pending_blocks_.erase(pending_it);
        }
    }

    for (size_t i = 0; i < request.new_block_ids.size(); ++i) {
        BlockId id = request.new_block_ids[i];
        file_it->second.blocks.push_back(id);
        auto block_it = blocks_.find(id);
        if (block_it != blocks_.end()) {
            block_it->second.state = BlockState::COMMITTED;
            if (i + 1 == request.new_block_ids.size()) {
                block_it->second.size = request.last_new_block_size;
                block_it->second.reserved_size = request.last_new_block_size;
            }
        }
    }

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    file_it->second.size += request.total_appended_bytes;
    file_it->second.modification_time = now;

    lock.unlock();
    block_commit_cv_.notify_all();
    return ErrorCode::OK;
}

std::vector<FileMetadata> NamespaceManager::all_files() const {
    std::shared_lock lock(mutex_);
    std::vector<FileMetadata> result;
    result.reserve(files_.size());
    for (const auto& [path, metadata] : files_) {
        (void)path;
        result.push_back(metadata);
    }
    return result;
}

std::vector<std::pair<BlockId, BlockMetadata>> NamespaceManager::all_blocks() const {
    std::shared_lock lock(mutex_);
    std::vector<std::pair<BlockId, BlockMetadata>> result;
    result.reserve(blocks_.size());
    for (const auto& [id, metadata] : blocks_) {
        result.emplace_back(id, metadata);
    }
    return result;
}

FileId NamespaceManager::next_file_id() const {
    std::shared_lock lock(mutex_);
    return next_file_id_;
}

BlockId NamespaceManager::next_block_id() const {
    std::shared_lock lock(mutex_);
    return next_block_id_;
}

void NamespaceManager::restore(std::vector<FileMetadata> files,
                                std::vector<std::pair<BlockId, BlockMetadata>> blocks,
                                FileId next_file_id, BlockId next_block_id) {
    std::unique_lock lock(mutex_);
    files_.clear();
    for (FileMetadata& file : files) {
        std::string path = file.path;
        files_.emplace(std::move(path), std::move(file));
    }
    blocks_.clear();
    for (auto& [id, metadata] : blocks) {
        blocks_.emplace(id, std::move(metadata));
    }
    pending_blocks_.clear();
    next_file_id_ = next_file_id;
    next_block_id_ = next_block_id;
}

Result<BlockMetadata> NamespaceManager::block_metadata(BlockId block_id) const {
    std::shared_lock lock(mutex_);
    auto it = blocks_.find(block_id);
    if (it == blocks_.end()) {
        return Result<BlockMetadata>::err(ErrorCode::BLOCK_NOT_FOUND);
    }
    return Result<BlockMetadata>::ok(it->second);
}
