#ifndef DFS_MASTER_METADATA_STORE_H_
#define DFS_MASTER_METADATA_STORE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/errors.h"
#include "master/metadata.h"
#include "master/storage_registry.h"

// Faza 12.4: Master's sole persistence mechanism — one combined binary
// snapshot file (<data_dir>/metadata.snapshot), written synchronously on
// every durable mutation via the same tmp+fsync+rename atomic-publish
// pattern as BlockStorage (see docs/design.md §16). In-flight
// RecoveryOperations are deliberately NOT persisted — post-restart
// reconciliation (NODE_INVENTORY, Faza 12.3) naturally rediscovers
// anything still under-replicated. NodeState/last_heartbeat are
// deliberately NOT persisted either — every node is restored as JOINING
// and only promoted back to HEALTHY through the NODE_INVENTORY mechanism.
class MetadataStore {
public:
    static Result<std::unique_ptr<MetadataStore>> create(std::filesystem::path data_dir);

    MetadataStore(const MetadataStore&) = delete;
    MetadataStore& operator=(const MetadataStore&) = delete;

    ErrorCode save(const std::vector<FileMetadata>& files,
                   const std::vector<std::pair<BlockId, BlockMetadata>>& blocks,
                   uint64_t next_file_id, uint64_t next_block_id,
                   const std::vector<StorageNodeInfo>& nodes);

    struct LoadedState {
        std::vector<FileMetadata> files;
        std::vector<std::pair<BlockId, BlockMetadata>> blocks;
        uint64_t next_file_id = 1;
        uint64_t next_block_id = 1;
        std::vector<StorageNodeInfo> nodes;
    };
    // A missing snapshot file returns an empty LoadedState as OK (a fresh
    // cluster). A CORRUPTED snapshot (bad magic/CRC, truncated, unreadable)
    // ALSO returns an empty LoadedState as OK rather than an error — same
    // "exclude what's damaged, keep serving" philosophy as
    // BlockStorage::recover()'s per-block checksum handling: a Master with
    // damaged metadata should still be able to start (losing its namespace
    // state) rather than refuse to boot entirely. Only a genuine directory-
    // level I/O failure at create() time is a hard error.
    Result<LoadedState> load();

private:
    explicit MetadataStore(std::filesystem::path data_dir);

    std::filesystem::path snapshot_path() const;
    std::filesystem::path temp_path() const;

    std::filesystem::path data_dir_;
    std::mutex mutex_;
};

#endif  // DFS_MASTER_METADATA_STORE_H_
