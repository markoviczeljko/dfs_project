#ifndef DFS_MASTER_FAILURE_DETECTOR_H_
#define DFS_MASTER_FAILURE_DETECTOR_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "common/config.h"
#include "common/errors.h"
#include "common/types.h"
#include "master/storage_registry.h"

// Faza 12.1: periodically sweeps StorageRegistry for nodes that have gone
// quiet — HEALTHY -> SUSPECTED past SUSPECTED_TIMEOUT_SECONDS since their
// last heartbeat, SUSPECTED -> DEAD past DEAD_TIMEOUT_SECONDS. See
// docs/design.md §16.
//
// A single background thread (own std::thread + condition_variable, same
// idiom as NetworkServer's accept-thread) rather than a shared
// "PeriodicTask" utility — only two call sites would ever use one (this
// and StorageNode's heartbeat sender), which doesn't clear this project's
// established "no abstraction without a real second beneficiary" bar (see
// Faza 11's decision not to add a gc.h/gc.cpp for the same reason).
class FailureDetector {
public:
    using NodeDeadCallback = std::function<void(StorageNodeId)>;

    // on_node_dead is invoked exactly once per HEALTHY/SUSPECTED -> DEAD
    // transition, from the sweep thread — callers must not block it for
    // long. A no-op default lets Faza 12.1 wire this up before Faza 12.2's
    // RecoveryManager exists to receive it.
    //
    // suspected_timeout/dead_timeout/sweep_interval default to the real
    // config.h constants; tests override them with small values instead of
    // waiting out SUSPECTED_TIMEOUT_SECONDS/DEAD_TIMEOUT_SECONDS for real.
    explicit FailureDetector(
        StorageRegistry& registry, NodeDeadCallback on_node_dead = [](StorageNodeId) {},
        std::chrono::milliseconds suspected_timeout =
            std::chrono::seconds(SUSPECTED_TIMEOUT_SECONDS),
        std::chrono::milliseconds dead_timeout = std::chrono::seconds(DEAD_TIMEOUT_SECONDS),
        std::chrono::milliseconds sweep_interval =
            std::chrono::milliseconds(FAILURE_SWEEP_INTERVAL_MS));

    FailureDetector(const FailureDetector&) = delete;
    FailureDetector& operator=(const FailureDetector&) = delete;

    ~FailureDetector();

    void start();
    void stop();

    // Wraps StorageRegistry::record_heartbeat — kept here too so callers
    // (Master::handle_heartbeat) only need to know about FailureDetector,
    // not reach past it into the registry it manages.
    ErrorCode process_heartbeat(StorageNodeId id);

    // Faza 12.3 bugfix: erases id's single-fire DEAD notification marker.
    // MUST be called by whatever promotes a node back out of DEAD
    // (currently only Master::handle_node_inventory()'s reconciliation),
    // or a SECOND failure of the same node silently never re-fires
    // on_node_dead_ again — see the NOTE this replaces above and
    // docs/design.md §16.
    void clear_notification(StorageNodeId id);

private:
    void sweep_loop();
    void check_timeouts();

    StorageRegistry& registry_;
    NodeDeadCallback on_node_dead_;
    std::chrono::milliseconds suspected_timeout_;
    std::chrono::milliseconds dead_timeout_;
    std::chrono::milliseconds sweep_interval_;

    std::thread sweep_thread_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};

    // A DEAD node stays DEAD until something promotes it back (Faza 12.3's
    // NODE_INVENTORY reconciliation) — this tracks "have we already fired
    // on_node_dead_ for this id" to guarantee single-fire without
    // re-notifying on every sweep tick between the DEAD transition and
    // that promotion. clear_notification() is the hook the promotion path
    // uses to reset this, so a SECOND failure of the same node still
    // fires on_node_dead_ again.
    std::mutex notified_mutex_;
    std::unordered_set<StorageNodeId> notified_dead_;
};

#endif  // DFS_MASTER_FAILURE_DETECTOR_H_
