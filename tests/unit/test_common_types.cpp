#include <gtest/gtest.h>

#include "common/config.h"
#include "common/errors.h"
#include "common/types.h"

TEST(CommonTypes, AreUint64) {
    FileId file_id = 1;
    BlockId block_id = 2;
    StorageNodeId node_id = 3;

    EXPECT_EQ(sizeof(file_id), sizeof(uint64_t));
    EXPECT_EQ(sizeof(block_id), sizeof(uint64_t));
    EXPECT_EQ(sizeof(node_id), sizeof(uint64_t));
}

TEST(CommonConfig, HasExpectedValues) {
    EXPECT_EQ(BLOCK_SIZE, 64ULL * 1024 * 1024);
    EXPECT_EQ(DEFAULT_REPLICATION_FACTOR, 3u);
}

TEST(Result, OkHoldsValue) {
    Result<int> r = Result<int>::ok(5);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 5);
}

TEST(Result, ErrHoldsErrorCode) {
    Result<int> r = Result<int>::err(ErrorCode::FILE_NOT_FOUND);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error(), ErrorCode::FILE_NOT_FOUND);
}
