#ifndef DFS_MASTER_RECOVERY_MANAGER_H_
#define DFS_MASTER_RECOVERY_MANAGER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "common/types.h"
#include "master/namespace.h"
#include "master/replication_manager.h"
#include "master/storage_registry.h"

// Faza 12.2: re-replication after a Storage Node is declared DEAD (see
// docs/design.md §16, §8.6-§8.16, §8.42, §8.46). A small worker pool
// (MAX_CONCURRENT_RECOVERY_OPERATIONS threads) pulls (block, failed_node)
// pairs off a shared queue and, for each: picks a healthy source replica
// and a healthy destination not already holding the block, pushes the
// data node-to-node via COPY_BLOCK/COPY_BLOCK_DATA, and on success updates
// NamespaceManager's replica list. Master itself never proxies block data
// (see docs/design.md §16, §8.8/§7.13).
//
// Deliberately a class separate from ReplicationManager (whose existing
// choose_replicas()-based public API stays untouched — see
// docs/design.md §16) rather than folding recovery orchestration into it.
class RecoveryManager {
public:
    RecoveryManager(NamespaceManager& namespace_manager, StorageRegistry& registry,
                     ReplicationManager& replication_manager,
                     std::function<void()> on_metadata_changed = [] {});

    RecoveryManager(const RecoveryManager&) = delete;
    RecoveryManager& operator=(const RecoveryManager&) = delete;

    ~RecoveryManager();

    void start();
    void stop();

    // Enqueues every block that had a replica on node_id for recovery —
    // typically wired as FailureDetector's on_node_dead callback (see
    // docs/design.md §16).
    void handle_node_failure(StorageNodeId node_id);

    // Faza 12.3: resyncs one specific block on one specific node whose
    // NODE_INVENTORY report showed it holding an outdated version — reuses
    // send_copy_block() with source = any OTHER healthy replica and
    // destination = stale_node itself (see docs/design.md §16,
    // Master::handle_node_inventory()). Synchronous (unlike
    // handle_node_failure(), which enqueues onto the worker pool) — a
    // single stale-replica resync is a bounded, one-shot operation, not a
    // batch that needs throttling against MAX_CONCURRENT_RECOVERY_OPERATIONS.
    // A no-op if the block is gone or no healthy source is currently
    // available — a later inventory report may retry.
    void handle_stale_replica(StorageNodeId stale_node, BlockId block_id, uint64_t expected_version);

private:
    struct RecoveryJob {
        BlockId block_id;
        StorageNodeId failed_node_id;
    };

    void worker_loop();
    // Retries in place, up to MAX_RECOVERY_ATTEMPTS_PER_BLOCK times, before
    // giving up on this job (leaving the block UNDER_REPLICATED — a later
    // heartbeat/inventory event may re-trigger recovery; see
    // docs/design.md §16, no default attempt bound exists in the design
    // docs for this, chosen here).
    void recover_block(RecoveryJob job);
    // Opens a connection to source, sends COPY_BLOCK naming destination,
    // and waits for the source's relayed result — bounded by
    // COPY_BLOCK_TIMEOUT_MS on this leg; the source->destination leg is
    // independently bounded the same way inside
    // StorageNode::handle_copy_block() (see docs/design.md §16, and the
    // Faza 11 CRITICAL-bug lesson this mirrors).
    bool send_copy_block(StorageNodeId source, BlockId block_id, uint64_t expected_version,
                          StorageNodeId destination, OperationId operation_id);
    OperationId next_operation_id();

    NamespaceManager& namespace_manager_;
    StorageRegistry& registry_;
    ReplicationManager& replication_manager_;
    std::function<void()> on_metadata_changed_;

    std::vector<std::thread> workers_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<RecoveryJob> queue_;
    std::atomic<bool> running_{false};

    std::mutex rng_mutex_;
    std::mt19937_64 operation_id_rng_{std::random_device{}()};
};

#endif  // DFS_MASTER_RECOVERY_MANAGER_H_
