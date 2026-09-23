#include "client/cache.h"

#include <utility>

ClientCache::ClientCache(size_t max_bytes) : max_bytes_(max_bytes) {}

std::optional<CacheEntry> ClientCache::get(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        ++stats_.misses;
        return std::nullopt;
    }
    ++stats_.hits;
    stats_.bytes_served += it->second.entry.data.size();
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_pos);
    it->second.lru_pos = lru_list_.begin();
    return it->second.entry;
}

void ClientCache::put(BlockId id, uint64_t version, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t new_size = data.size();

    auto existing = entries_.find(id);
    if (existing != entries_.end()) {
        current_bytes_ -= existing->second.entry.data.size();
        lru_list_.erase(existing->second.lru_pos);
        entries_.erase(existing);
    }

    if (max_bytes_ == 0) {
        // A zero-capacity cache stores nothing at all. evict_locked() alone
        // cannot enforce this: with an empty lru_list_ it has nothing to
        // evict against, so the entry below would otherwise be inserted and
        // then linger until a *different* block id arrived to evict it.
        return;
    }

    evict_locked(new_size);

    lru_list_.push_front(id);
    CacheEntry entry{id, version, std::move(data)};
    entries_[id] = Entry{std::move(entry), lru_list_.begin()};
    current_bytes_ += new_size;
}

void ClientCache::invalidate(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return;
    }
    current_bytes_ -= it->second.entry.data.size();
    lru_list_.erase(it->second.lru_pos);
    entries_.erase(it);
    ++stats_.invalidations;
}

void ClientCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_list_.clear();
    current_bytes_ = 0;
}

CacheStats ClientCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void ClientCache::evict_locked(size_t needed_bytes) {
    while (current_bytes_ + needed_bytes > max_bytes_ && !lru_list_.empty()) {
        BlockId victim = lru_list_.back();
        auto it = entries_.find(victim);
        current_bytes_ -= it->second.entry.data.size();
        lru_list_.pop_back();
        entries_.erase(it);
        ++stats_.evictions;
    }
}
