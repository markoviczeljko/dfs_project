#include "client/cache.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

std::vector<uint8_t> make_data(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

}  // namespace

TEST(CacheTest, MissOnEmptyCache) {
    ClientCache cache(1024);
    EXPECT_FALSE(cache.get(1).has_value());
    EXPECT_EQ(cache.stats().misses, 1u);
}

TEST(CacheTest, PutThenGetReturnsSameDataAndVersion) {
    ClientCache cache(1024);
    cache.put(1, /*version=*/7, make_data(10, 0xAB));

    auto entry = cache.get(1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->block_id, 1u);
    EXPECT_EQ(entry->version, 7u);
    EXPECT_EQ(entry->data, make_data(10, 0xAB));
    EXPECT_EQ(cache.stats().hits, 1u);
}

TEST(CacheTest, InvalidateRemovesEntry) {
    ClientCache cache(1024);
    cache.put(1, 1, make_data(10, 0x01));
    cache.invalidate(1);

    EXPECT_FALSE(cache.get(1).has_value());
    EXPECT_EQ(cache.stats().invalidations, 1u);
}

TEST(CacheTest, InvalidateOnMissingEntryIsANoOp) {
    ClientCache cache(1024);
    cache.invalidate(42);  // must not throw/crash
    EXPECT_EQ(cache.stats().invalidations, 0u);
}

TEST(CacheTest, ClearRemovesEverything) {
    ClientCache cache(1024);
    cache.put(1, 1, make_data(10, 0x01));
    cache.put(2, 1, make_data(10, 0x02));
    cache.clear();

    EXPECT_FALSE(cache.get(1).has_value());
    EXPECT_FALSE(cache.get(2).has_value());
}

TEST(CacheTest, PutOverwritesExistingEntryWithNewVersionAndData) {
    ClientCache cache(1024);
    cache.put(1, 1, make_data(10, 0x01));
    cache.put(1, 2, make_data(20, 0x02));

    auto entry = cache.get(1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->version, 2u);
    EXPECT_EQ(entry->data, make_data(20, 0x02));
}

// Design notes §9.15's exact walkthrough: cache holds A, B, C, D; access
// order A, C, A, B leaves D as least-recently-used; inserting E evicts D.
TEST(CacheTest, LruEvictsLeastRecentlyUsedBlock) {
    constexpr size_t kEntrySize = 100;
    ClientCache cache(4 * kEntrySize);  // room for exactly 4 entries

    cache.put(/*A=*/1, 1, make_data(kEntrySize, 'A'));
    cache.put(/*B=*/2, 1, make_data(kEntrySize, 'B'));
    cache.put(/*C=*/3, 1, make_data(kEntrySize, 'C'));
    cache.put(/*D=*/4, 1, make_data(kEntrySize, 'D'));

    ASSERT_TRUE(cache.get(/*A=*/1).has_value());
    ASSERT_TRUE(cache.get(/*C=*/3).has_value());
    ASSERT_TRUE(cache.get(/*A=*/1).has_value());
    ASSERT_TRUE(cache.get(/*B=*/2).has_value());

    cache.put(/*E=*/5, 1, make_data(kEntrySize, 'E'));

    EXPECT_FALSE(cache.get(/*D=*/4).has_value()) << "D should have been evicted";
    EXPECT_TRUE(cache.get(/*A=*/1).has_value());
    EXPECT_TRUE(cache.get(/*B=*/2).has_value());
    EXPECT_TRUE(cache.get(/*C=*/3).has_value());
    EXPECT_TRUE(cache.get(/*E=*/5).has_value());
    EXPECT_EQ(cache.stats().evictions, 1u);
}

TEST(CacheTest, EvictsMultipleEntriesIfNeededToFitOneLargeEntry) {
    constexpr size_t kEntrySize = 100;
    ClientCache cache(4 * kEntrySize);

    cache.put(1, 1, make_data(kEntrySize, 'A'));
    cache.put(2, 1, make_data(kEntrySize, 'B'));
    cache.put(3, 1, make_data(kEntrySize, 'C'));
    cache.put(4, 1, make_data(kEntrySize, 'D'));

    // A new entry spanning 3 slots must evict 3 LRU entries (1, 2, 3, in
    // that order) to fit, leaving only the most recent (4) behind.
    cache.put(5, 1, make_data(3 * kEntrySize, 'E'));

    EXPECT_FALSE(cache.get(1).has_value());
    EXPECT_FALSE(cache.get(2).has_value());
    EXPECT_FALSE(cache.get(3).has_value());
    EXPECT_TRUE(cache.get(4).has_value());
    EXPECT_TRUE(cache.get(5).has_value());
}

// Edge case (see docs/design.md §17): a single entry larger than the whole
// cache capacity is still cached alone rather than rejected.
TEST(CacheTest, EntryLargerThanCapacityIsStillCached) {
    ClientCache cache(10);
    cache.put(1, 1, make_data(100, 'A'));

    auto entry = cache.get(1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->data.size(), 100u);
}

TEST(CacheTest, StatsTrackHitsMissesEvictionsInvalidationsAndBytesServed) {
    constexpr size_t kEntrySize = 10;
    ClientCache cache(kEntrySize);  // room for exactly 1 entry

    cache.get(1);                                 // miss
    cache.put(1, 1, make_data(kEntrySize, 'A'));   // no eviction (empty before)
    cache.get(1);                                  // hit, +10 bytes served
    cache.put(2, 1, make_data(kEntrySize, 'B'));   // evicts 1
    cache.invalidate(2);                            // +1 invalidation

    CacheStats stats = cache.stats();
    EXPECT_EQ(stats.misses, 1u);
    EXPECT_EQ(stats.hits, 1u);
    EXPECT_EQ(stats.bytes_served, kEntrySize);
    EXPECT_EQ(stats.evictions, 1u);
    EXPECT_EQ(stats.invalidations, 1u);
}
