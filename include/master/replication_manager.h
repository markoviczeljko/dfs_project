#ifndef DFS_MASTER_REPLICATION_MANAGER_H_
#define DFS_MASTER_REPLICATION_MANAGER_H_

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include "common/types.h"
#include "master/storage_registry.h"

// Faza 9: replica placement. See docs/design.md §13.
//
// choose_replicas() picks DEFAULT_REPLICATION_FACTOR distinct alive nodes
// via a deterministic round-robin cursor over get_alive_nodes() sorted by
// id — not random selection. This is a diploma-thesis-appropriate choice:
// it gives "relatively even" distribution (the design notes' only stated
// requirement) as a side effect, guarantees distinct nodes whenever enough
// are alive, and is exactly reproducible in unit tests, unlike a
// hash/random policy (no <random> usage exists anywhere else in this
// codebase). Returns an empty vector if fewer than the replication factor
// are currently alive — the caller (Master::handle_allocate_block) treats
// that as NODE_NOT_FOUND rather than allocating a permanently
// under-replicated block this phase has no way to ever repair (repair is
// Faza 12).
//
// replicate_block()/handle_node_failure() from the design sketch (§17.8)
// are deliberately not declared here — no caller exists in this phase
// (no heartbeat/failure-detection, no re-replication), matching the
// project's established "no value-less stub" precedent.
class ReplicationManager {
public:
    explicit ReplicationManager(StorageRegistry& registry);

    ReplicationManager(const ReplicationManager&) = delete;
    ReplicationManager& operator=(const ReplicationManager&) = delete;

    std::vector<StorageNodeId> choose_replicas(BlockId block_id);

    // Faza 12.2: picks one HEALTHY node not already in exclude (typically
    // the block's current replica set) as a recovery destination — reuses
    // get_alive_nodes() rather than inventing separate placement logic
    // (see docs/design.md §16). Returns nullopt if every healthy node is
    // already excluded.
    std::optional<StorageNodeId> choose_recovery_destination(
        const std::vector<StorageNodeId>& exclude);

private:
    StorageRegistry& registry_;
    std::mutex mutex_;
    size_t cursor_ = 0;
};

#endif  // DFS_MASTER_REPLICATION_MANAGER_H_
