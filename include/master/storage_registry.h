#ifndef DFS_MASTER_STORAGE_REGISTRY_H_
#define DFS_MASTER_STORAGE_REGISTRY_H_

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

// Faza 9: node inventory tracked by Master. See docs/design.md §13.
//
// free_space is populated once, from total_space, at registration time and
// never updated afterward — there is no heartbeat mechanism yet (Faza 12)
// to report live usage, so it is a deliberately frozen snapshot, not a
// live value. Placement (ReplicationManager::choose_replicas) does not
// filter on it in this phase.
struct StorageNodeInfo {
    StorageNodeId id;
    std::string host;
    uint16_t port;
    bool alive;
    uint64_t total_space;
    uint64_t free_space;
};

// In-memory registry of known Storage Nodes: id -> StorageNodeInfo.
// register_node() is an idempotent upsert (a node re-registering, e.g.
// after a manual restart during development, simply overwrites its prior
// entry — see docs/design.md §13). mark_dead()/mark_alive() and the
// "alive" flag exist per the design sketch (§17.9) even though no Faza 9
// code path calls them yet (no heartbeat/failure-detection exists until
// Faza 12) — they are real, spec'd API surface, not stubs.
class StorageRegistry {
public:
    StorageRegistry() = default;

    StorageRegistry(const StorageRegistry&) = delete;
    StorageRegistry& operator=(const StorageRegistry&) = delete;

    void register_node(StorageNodeInfo node);
    void mark_dead(StorageNodeId id);
    void mark_alive(StorageNodeId id);
    std::vector<StorageNodeInfo> get_alive_nodes() const;
    std::optional<StorageNodeInfo> get_node(StorageNodeId id) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<StorageNodeId, StorageNodeInfo> nodes_;
};

#endif  // DFS_MASTER_STORAGE_REGISTRY_H_
