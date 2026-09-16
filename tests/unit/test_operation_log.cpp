#include "storage/operation_log.h"

#include <gtest/gtest.h>

#include <vector>

TEST(OperationLogTest, BeginOrGetOnNewIdReturnsNullopt) {
    OperationLog log;
    EXPECT_FALSE(log.begin_or_get(1).has_value());
}

TEST(OperationLogTest, BeginOrGetOnInProgressIdReturnsInProgressRecord) {
    OperationLog log;
    ASSERT_FALSE(log.begin_or_get(1).has_value());

    auto record = log.begin_or_get(1);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, OperationState::IN_PROGRESS);
}

TEST(OperationLogTest, CompleteThenBeginOrGetReplaysStoredResult) {
    OperationLog log;
    ASSERT_FALSE(log.begin_or_get(1).has_value());

    std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}};
    log.complete(1, ErrorCode::OK, payload);

    auto record = log.begin_or_get(1);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, OperationState::COMPLETED);
    EXPECT_EQ(record->result, ErrorCode::OK);
    EXPECT_EQ(record->response_payload, payload);
}

TEST(OperationLogTest, CompleteWithFailureIsRememberedAsFailed) {
    OperationLog log;
    ASSERT_FALSE(log.begin_or_get(1).has_value());
    log.complete(1, ErrorCode::BLOCK_CORRUPTED, {});

    auto record = log.begin_or_get(1);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, OperationState::FAILED);
    EXPECT_EQ(record->result, ErrorCode::BLOCK_CORRUPTED);
}

TEST(OperationLogTest, DistinctIdsAreIndependent) {
    OperationLog log;
    ASSERT_FALSE(log.begin_or_get(1).has_value());
    log.complete(1, ErrorCode::OK, {});

    EXPECT_FALSE(log.begin_or_get(2).has_value());
}

TEST(OperationLogTest, EvictsOldestEntryPastCapacity) {
    OperationLog log(/*capacity=*/2);
    ASSERT_FALSE(log.begin_or_get(1).has_value());
    log.complete(1, ErrorCode::OK, {});
    ASSERT_FALSE(log.begin_or_get(2).has_value());
    log.complete(2, ErrorCode::OK, {});
    // Pushes id 1 out of the bounded FIFO.
    ASSERT_FALSE(log.begin_or_get(3).has_value());
    log.complete(3, ErrorCode::OK, {});

    // ids 2 and 3 are still remembered (checked first — begin_or_get(1)
    // below itself inserts and would evict one of them).
    auto record2 = log.begin_or_get(2);
    ASSERT_TRUE(record2.has_value());
    EXPECT_EQ(record2->state, OperationState::COMPLETED);

    // id 1 was evicted — treated as brand-new again, not as a replay.
    EXPECT_FALSE(log.begin_or_get(1).has_value());
}
