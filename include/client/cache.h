#ifndef DFS_CLIENT_CACHE_H_
#define DFS_CLIENT_CACHE_H_

#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/types.h"

// Faza 13: block-level LRU cache living inside a single Client process (see
// docs/design.md §17, design notes §9). Pure optimization, never the source
// of truth — losing the cache just means re-fetching on the next miss.
// Deliberately provides no cross-client coherence (design notes §9.13):
// invalidate() is the only staleness defense, called by Client on its own
// successful WRITE/APPEND.
struct CacheEntry {
    BlockId block_id;
    uint64_t version;
    std::vector<uint8_t> data;
};

struct CacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;
    uint64_t invalidations = 0;
    uint64_t bytes_served = 0;
};

class ClientCache {
public:
    explicit ClientCache(size_t max_bytes);

    // Returns a copy of the cached entry and marks it most-recently-used,
    // or nullopt on a miss. Never performs I/O. The block's data is copied
    // while mutex_ is held (needed for a consistent snapshot — the
    // alternative, returning a reference/pointer into entries_, would let
    // a concurrent invalidate()/evict race dangle it), so a large hit
    // briefly serializes other cache access. Fine for the current
    // documented use (Client is single-threaded, see client.h); revisit
    // with e.g. a shared_ptr<const vector<uint8_t>> payload if ClientCache
    // is ever shared across threads.
    //
    // Faza 15 audit (design notes §9.20/§11.29 — "unlock before network
    // I/O"): confirmed every call site in client.cpp (get/put/invalidate,
    // ~5 sites) is a single, self-contained call — none hold onto
    // anything returned from mutex_ across a network round-trip, since
    // get()/put() never return a reference into entries_ in the first
    // place. Structurally guaranteed by this API, not just by convention.
    std::optional<CacheEntry> get(BlockId id);

    // Inserts/replaces the entry for id, evicting least-recently-used
    // entries (from the back of the LRU list) until it fits within
    // max_bytes_. A single entry larger than max_bytes_ is still cached
    // alone (see docs/design.md §17 — deliberate simplification, matches
    // no other entry existing to evict it against).
    void put(BlockId id, uint64_t version, std::vector<uint8_t> data);

    void invalidate(BlockId id);
    void clear();

    CacheStats stats() const;

private:
    struct Entry {
        CacheEntry entry;
        std::list<BlockId>::iterator lru_pos;
    };

    void evict_locked(size_t needed_bytes);

    mutable std::mutex mutex_;
    size_t max_bytes_;
    size_t current_bytes_ = 0;
    std::list<BlockId> lru_list_;  // front = most-recently-used, back = least
    std::unordered_map<BlockId, Entry> entries_;
    CacheStats stats_;
};

#endif  // DFS_CLIENT_CACHE_H_
