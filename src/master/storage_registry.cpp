#include "master/storage_registry.h"

#include <mutex>

void StorageRegistry::register_node(StorageNodeInfo node) {
    std::unique_lock lock(mutex_);
    nodes_[node.id] = std::move(node);
}

void StorageRegistry::mark_dead(StorageNodeId id) { set_state(id, NodeState::DEAD); }

void StorageRegistry::mark_alive(StorageNodeId id) { set_state(id, NodeState::HEALTHY); }

void StorageRegistry::set_state(StorageNodeId id, NodeState state) {
    std::unique_lock lock(mutex_);
    auto it = nodes_.find(id);
    if (it != nodes_.end()) {
        it->second.state = state;
    }
}

ErrorCode StorageRegistry::record_heartbeat(StorageNodeId id) {
    std::unique_lock lock(mutex_);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return ErrorCode::NODE_NOT_FOUND;
    }
    it->second.last_heartbeat = std::chrono::steady_clock::now();
    if (it->second.state == NodeState::SUSPECTED) {
        it->second.state = NodeState::HEALTHY;
    }
    return ErrorCode::OK;
}

std::vector<StorageNodeInfo> StorageRegistry::get_alive_nodes() const {
    return get_nodes_in_state(NodeState::HEALTHY);
}

std::vector<StorageNodeInfo> StorageRegistry::get_nodes_in_state(NodeState state) const {
    std::shared_lock lock(mutex_);
    std::vector<StorageNodeInfo> matching;
    for (const auto& [id, node] : nodes_) {
        if (node.state == state) {
            matching.push_back(node);
        }
    }
    return matching;
}

std::vector<StorageNodeInfo> StorageRegistry::get_all_nodes() const {
    std::shared_lock lock(mutex_);
    std::vector<StorageNodeInfo> all;
    all.reserve(nodes_.size());
    for (const auto& [id, node] : nodes_) {
        all.push_back(node);
    }
    return all;
}

void StorageRegistry::restore_node(StorageNodeInfo node) {
    node.state = NodeState::JOINING;
    node.last_heartbeat = std::chrono::steady_clock::time_point{};
    std::unique_lock lock(mutex_);
    nodes_[node.id] = std::move(node);
}

std::optional<StorageNodeInfo> StorageRegistry::get_node(StorageNodeId id) const {
    std::shared_lock lock(mutex_);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return std::nullopt;
    }
    return it->second;
}
