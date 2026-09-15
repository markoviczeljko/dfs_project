#include "master/replication_manager.h"

#include <algorithm>

#include "common/config.h"

ReplicationManager::ReplicationManager(StorageRegistry& registry) : registry_(registry) {}

std::vector<StorageNodeId> ReplicationManager::choose_replicas(BlockId /*block_id*/) {
    std::vector<StorageNodeInfo> alive = registry_.get_alive_nodes();
    std::sort(alive.begin(), alive.end(),
              [](const StorageNodeInfo& a, const StorageNodeInfo& b) { return a.id < b.id; });

    if (alive.size() < DEFAULT_REPLICATION_FACTOR) {
        return {};
    }

    std::unique_lock lock(mutex_);
    std::vector<StorageNodeId> chosen;
    chosen.reserve(DEFAULT_REPLICATION_FACTOR);
    for (size_t i = 0; i < DEFAULT_REPLICATION_FACTOR; ++i) {
        chosen.push_back(alive[(cursor_ + i) % alive.size()].id);
    }
    cursor_ += DEFAULT_REPLICATION_FACTOR;
    return chosen;
}
