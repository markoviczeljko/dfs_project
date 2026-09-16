#ifndef DFS_MASTER_STORAGE_REGISTRY_H_
#define DFS_MASTER_STORAGE_REGISTRY_H_

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/errors.h"
#include "common/types.h"

// Faza 12: node liveness state machine (see docs/design.md §16). Absorbs
// the design sketch's separate "NodeManager" (§7.22) directly into
// StorageRegistry rather than a parallel node map — this class already
// owns node identity, and a second map would just be two sources of truth
// for the same id space (see docs/design.md §16 for the full rationale,
// matching Faza 11's precedent of not introducing a class for something
// with no independent state beyond what an existing one already owns).
//
// JOINING -> HEALTHY -> (heartbeat timeout) -> SUSPECTED -> (timeout
// continues) -> DEAD -> (node reports its inventory) -> HEALTHY.
// RECOVERING is reserved for Faza 12.3/12.4 (a DEAD node's blocks are
// being actively re-replicated elsewhere while it's absent) but not yet
// transitioned into/out of by this sub-phase.
enum class NodeState { JOINING, HEALTHY, SUSPECTED, DEAD, RECOVERING };

// free_space is populated once, from total_space, at registration time and
// never updated afterward — no code path reports live usage. Placement
// (ReplicationManager::choose_replicas) does not filter on it.
struct StorageNodeInfo {
    StorageNodeId id;
    std::string host;
    uint16_t port;
    NodeState state;
    uint64_t total_space;
    uint64_t free_space;
    // Only meaningful once state has left JOINING for the first time;
    // record_heartbeat() is what actually advances it. See
    // FailureDetector::check_timeouts() for how this drives the state
    // machine above.
    std::chrono::steady_clock::time_point last_heartbeat;
};

// In-memory registry of known Storage Nodes: id -> StorageNodeInfo.
// register_node() is an idempotent upsert (a node re-registering, e.g.
// after a manual restart during development, simply overwrites its prior
// entry — see docs/design.md §13/§16). It does not decide the node's
// initial state itself — that's the caller's call: Master constructs a
// fresh registration as HEALTHY (the REGISTER_NODE round-trip itself is
// live proof of reachability, see Master::handle_register_node), while
// restoring a persisted snapshot after a Master restart (Faza 12.4)
// deliberately uses JOINING instead (see StorageRegistry::restore_node,
// added in 12.4) — a Master that just recovered has no live proof any
// previously-known node is still up until it reports its inventory.
class StorageRegistry {
public:
    StorageRegistry() = default;

    StorageRegistry(const StorageRegistry&) = delete;
    StorageRegistry& operator=(const StorageRegistry&) = delete;

    void register_node(StorageNodeInfo node);

    // Faza 12: thin wrappers around set_state(id, DEAD/HEALTHY) — kept for
    // source compatibility with the design sketch's §17.9 API surface.
    void mark_dead(StorageNodeId id);
    void mark_alive(StorageNodeId id);

    void set_state(StorageNodeId id, NodeState state);
    // Updates last_heartbeat; if the node was SUSPECTED, promotes it back
    // to HEALTHY (still within its failure window, so a late-but-arriving
    // heartbeat un-suspects it). Does NOT promote out of DEAD/RECOVERING —
    // that only ever happens via inventory reconciliation (Faza 12.3),
    // never from a bare heartbeat alone. NODE_NOT_FOUND if id is unknown.
    ErrorCode record_heartbeat(StorageNodeId id);

    // get_alive_nodes() keeps its Faza-9 name/signature (ReplicationManager
    // depends on both) but now means state == HEALTHY specifically, not
    // "any non-DEAD state" — matches the design's requirement that a
    // SUSPECTED node (unconfirmed, not yet failed, but not confirmed
    // reachable either) never becomes a fresh replica target.
    std::vector<StorageNodeInfo> get_alive_nodes() const;
    std::vector<StorageNodeInfo> get_nodes_in_state(NodeState state) const;
    std::vector<StorageNodeInfo> get_all_nodes() const;
    std::optional<StorageNodeInfo> get_node(StorageNodeId id) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<StorageNodeId, StorageNodeInfo> nodes_;
};

#endif  // DFS_MASTER_STORAGE_REGISTRY_H_
