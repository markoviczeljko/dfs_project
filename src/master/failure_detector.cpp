#include "master/failure_detector.h"

FailureDetector::FailureDetector(StorageRegistry& registry, NodeDeadCallback on_node_dead,
                                  std::chrono::milliseconds suspected_timeout,
                                  std::chrono::milliseconds dead_timeout,
                                  std::chrono::milliseconds sweep_interval)
    : registry_(registry),
      on_node_dead_(std::move(on_node_dead)),
      suspected_timeout_(suspected_timeout),
      dead_timeout_(dead_timeout),
      sweep_interval_(sweep_interval) {}

FailureDetector::~FailureDetector() { stop(); }

void FailureDetector::start() {
    if (running_.exchange(true)) return;  // already running
    sweep_thread_ = std::thread(&FailureDetector::sweep_loop, this);
}

void FailureDetector::stop() {
    if (!running_.exchange(false)) return;  // already stopped
    cv_.notify_all();
    if (sweep_thread_.joinable()) {
        sweep_thread_.join();
    }
}

ErrorCode FailureDetector::process_heartbeat(StorageNodeId id) {
    return registry_.record_heartbeat(id);
}

void FailureDetector::sweep_loop() {
    std::unique_lock lock(cv_mutex_);
    while (running_.load()) {
        // cv_.wait_for returns immediately on stop()'s notify_all() rather
        // than blocking out the full sweep_interval_ — matters because
        // dozens of Master instances start/stop per test run.
        cv_.wait_for(lock, sweep_interval_, [this] { return !running_.load(); });
        if (!running_.load()) break;
        lock.unlock();
        check_timeouts();
        lock.lock();
    }
}

void FailureDetector::check_timeouts() {
    auto now = std::chrono::steady_clock::now();

    for (const StorageNodeInfo& node : registry_.get_nodes_in_state(NodeState::HEALTHY)) {
        if (now - node.last_heartbeat >= suspected_timeout_) {
            registry_.set_state(node.id, NodeState::SUSPECTED);
        }
    }

    for (const StorageNodeInfo& node : registry_.get_nodes_in_state(NodeState::SUSPECTED)) {
        if (now - node.last_heartbeat >= dead_timeout_) {
            registry_.set_state(node.id, NodeState::DEAD);

            bool first_time;
            {
                std::lock_guard<std::mutex> notified_lock(notified_mutex_);
                first_time = notified_dead_.insert(node.id).second;
            }
            if (first_time) {
                on_node_dead_(node.id);
            }
        }
    }
}
