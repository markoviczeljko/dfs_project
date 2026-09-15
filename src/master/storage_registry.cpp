#include "master/storage_registry.h"

#include <mutex>

void StorageRegistry::register_node(StorageNodeInfo node) {
    std::unique_lock lock(mutex_);
    nodes_[node.id] = std::move(node);
}

void StorageRegistry::mark_dead(StorageNodeId id) {
    std::unique_lock lock(mutex_);
    auto it = nodes_.find(id);
    if (it != nodes_.end()) {
        it->second.alive = false;
    }
}

void StorageRegistry::mark_alive(StorageNodeId id) {
    std::unique_lock lock(mutex_);
    auto it = nodes_.find(id);
    if (it != nodes_.end()) {
        it->second.alive = true;
    }
}

std::vector<StorageNodeInfo> StorageRegistry::get_alive_nodes() const {
    std::shared_lock lock(mutex_);
    std::vector<StorageNodeInfo> alive;
    for (const auto& [id, node] : nodes_) {
        if (node.alive) {
            alive.push_back(node);
        }
    }
    return alive;
}

std::optional<StorageNodeInfo> StorageRegistry::get_node(StorageNodeId id) const {
    std::shared_lock lock(mutex_);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return std::nullopt;
    }
    return it->second;
}
