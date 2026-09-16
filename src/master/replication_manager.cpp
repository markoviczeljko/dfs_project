#include "master/replication_manager.h"

#include <algorithm>

ReplicationManager::ReplicationManager(StorageRegistry& registry, uint32_t replication_factor)
    : registry_(registry), replication_factor_(replication_factor) {}

std::vector<StorageNodeId> ReplicationManager::choose_replicas(BlockId /*block_id*/) {
    std::vector<StorageNodeInfo> alive = registry_.get_alive_nodes();
    std::sort(alive.begin(), alive.end(),
              [](const StorageNodeInfo& a, const StorageNodeInfo& b) { return a.id < b.id; });

    if (alive.size() < replication_factor_) {
        return {};
    }

    std::unique_lock lock(mutex_);
    std::vector<StorageNodeId> chosen;
    chosen.reserve(replication_factor_);
    for (size_t i = 0; i < replication_factor_; ++i) {
        chosen.push_back(alive[(cursor_ + i) % alive.size()].id);
    }
    cursor_ += replication_factor_;
    return chosen;
}

std::optional<StorageNodeId> ReplicationManager::choose_recovery_destination(
    const std::vector<StorageNodeId>& exclude) {
    std::vector<StorageNodeInfo> alive = registry_.get_alive_nodes();
    std::sort(alive.begin(), alive.end(),
              [](const StorageNodeInfo& a, const StorageNodeInfo& b) { return a.id < b.id; });

    for (const StorageNodeInfo& node : alive) {
        if (std::find(exclude.begin(), exclude.end(), node.id) == exclude.end()) {
            return node.id;
        }
    }
    return std::nullopt;
}
