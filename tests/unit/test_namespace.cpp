#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "common/config.h"
#include "master/namespace.h"

TEST(NamespaceManagerTest, CreateThenStatReturnsMetadata) {
    NamespaceManager ns;
    ASSERT_EQ(ns.create_file("/a.txt"), ErrorCode::OK);

    auto stat_result = ns.stat_file("/a.txt");
    ASSERT_TRUE(stat_result.is_ok());
    EXPECT_EQ(stat_result.value().path, "/a.txt");
    EXPECT_EQ(stat_result.value().size, 0u);
    EXPECT_TRUE(stat_result.value().blocks.empty());
}

TEST(NamespaceManagerTest, CreateOnExistingPathFails) {
    NamespaceManager ns;
    ASSERT_EQ(ns.create_file("/a.txt"), ErrorCode::OK);
    EXPECT_EQ(ns.create_file("/a.txt"), ErrorCode::FILE_ALREADY_EXISTS);
}

TEST(NamespaceManagerTest, DeleteThenStatFails) {
    NamespaceManager ns;
    ASSERT_EQ(ns.create_file("/a.txt"), ErrorCode::OK);
    ASSERT_EQ(ns.delete_file("/a.txt"), ErrorCode::OK);

    auto stat_result = ns.stat_file("/a.txt");
    ASSERT_TRUE(stat_result.is_error());
    EXPECT_EQ(stat_result.error(), ErrorCode::FILE_NOT_FOUND);
}

TEST(NamespaceManagerTest, DeleteNonexistentFails) {
    NamespaceManager ns;
    EXPECT_EQ(ns.delete_file("/never-created.txt"), ErrorCode::FILE_NOT_FOUND);
}

TEST(NamespaceManagerTest, StatNonexistentFails) {
    NamespaceManager ns;
    auto stat_result = ns.stat_file("/never-created.txt");
    ASSERT_TRUE(stat_result.is_error());
    EXPECT_EQ(stat_result.error(), ErrorCode::FILE_NOT_FOUND);
}

TEST(NamespaceManagerTest, ListFilesReturnsDirectChildren) {
    NamespaceManager ns;
    ASSERT_EQ(ns.create_file("/data/a.txt"), ErrorCode::OK);
    ASSERT_EQ(ns.create_file("/data/b.txt"), ErrorCode::OK);
    ASSERT_EQ(ns.create_file("/data/sub/c.txt"), ErrorCode::OK);

    auto list_result = ns.list_files("/data");
    ASSERT_TRUE(list_result.is_ok());
    std::vector<std::string> children = list_result.value();
    std::sort(children.begin(), children.end());
    EXPECT_EQ(children, (std::vector<std::string>{"a.txt", "b.txt", "sub"}));
}

TEST(NamespaceManagerTest, ListRootReturnsTopLevelEntries) {
    NamespaceManager ns;
    ASSERT_EQ(ns.create_file("/a.txt"), ErrorCode::OK);
    ASSERT_EQ(ns.create_file("/dir/b.txt"), ErrorCode::OK);

    auto list_result = ns.list_files("/");
    ASSERT_TRUE(list_result.is_ok());
    std::vector<std::string> children = list_result.value();
    std::sort(children.begin(), children.end());
    EXPECT_EQ(children, (std::vector<std::string>{"a.txt", "dir"}));
}

TEST(NamespaceManagerTest, ListNonexistentPrefixReturnsEmpty) {
    NamespaceManager ns;
    auto list_result = ns.list_files("/nope");
    ASSERT_TRUE(list_result.is_ok());
    EXPECT_TRUE(list_result.value().empty());
}

TEST(NamespaceManagerTest, InvalidPathRejected) {
    NamespaceManager ns;
    EXPECT_EQ(ns.create_file("relative.txt"), ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(ns.create_file("/a//b"), ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(ns.create_file("/"), ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(ns.create_file("/a/../b"), ErrorCode::INVALID_REQUEST);
}

// Faza 10: resolve_append_target()/commit_append() (see docs/design.md §14).
// Helper: creates /a.txt with one committed block of last_block_size bytes,
// replicated to {1, 2, 3}, and returns that block's id.
namespace {
BlockId create_file_with_one_block(NamespaceManager& ns, uint64_t last_block_size) {
    ns.create_file("/a.txt");
    auto block_result = ns.allocate_block("/a.txt");
    BlockId block_id = block_result.value();
    ns.set_block_replicas(block_id, {1, 2, 3});
    ns.commit_write("/a.txt", {block_id}, last_block_size, last_block_size);
    return block_id;
}
}  // namespace

TEST(NamespaceManagerTest, ResolveAppendTargetOnEmptyFileReturnsNoTarget) {
    NamespaceManager ns;
    ASSERT_EQ(ns.create_file("/a.txt"), ErrorCode::OK);

    auto result = ns.resolve_append_target("/a.txt", 10);
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value().has_target);
}

TEST(NamespaceManagerTest, ResolveAppendTargetOnNonexistentFileFails) {
    NamespaceManager ns;
    auto result = ns.resolve_append_target("/never-created.txt", 10);
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error(), ErrorCode::FILE_NOT_FOUND);
}

TEST(NamespaceManagerTest, ResolveAppendTargetFitsInLastBlockReturnsCorrectOffsetAndReplicas) {
    NamespaceManager ns;
    BlockId block_id = create_file_with_one_block(ns, /*last_block_size=*/100);

    auto result = ns.resolve_append_target("/a.txt", 50);
    ASSERT_TRUE(result.is_ok());
    const AppendTarget& target = result.value();
    EXPECT_TRUE(target.has_target);
    EXPECT_EQ(target.block_id, block_id);
    EXPECT_EQ(target.offset, 100u);
    EXPECT_EQ(target.fill_amount, 50u);
    EXPECT_EQ(target.replicas, (std::vector<StorageNodeId>{1, 2, 3}));
}

TEST(NamespaceManagerTest, ResolveAppendTargetClampsFillToFreeSpace) {
    NamespaceManager ns;
    create_file_with_one_block(ns, /*last_block_size=*/BLOCK_SIZE - 10);

    auto result = ns.resolve_append_target("/a.txt", 1000);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().has_target);
    EXPECT_EQ(result.value().fill_amount, 10u);
}

TEST(NamespaceManagerTest, ResolveAppendTargetWhenLastBlockFullReturnsNoTarget) {
    NamespaceManager ns;
    create_file_with_one_block(ns, /*last_block_size=*/BLOCK_SIZE);

    auto result = ns.resolve_append_target("/a.txt", 10);
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value().has_target);
}

TEST(NamespaceManagerTest, ResolveAppendTargetReservationsAreSerializedNotOverlapping) {
    // Repeated calls (simulating concurrent appends) must each see the
    // PREVIOUS call's reservation, never the same offset twice — this is
    // the optimistic-reservation mechanism that serializes concurrent
    // appends without holding the lock across I/O (see docs/design.md §14).
    NamespaceManager ns;
    create_file_with_one_block(ns, /*last_block_size=*/0);

    constexpr int kThreadCount = 8;
    constexpr uint64_t kPerCallSize = 10;
    std::vector<uint64_t> offsets(kThreadCount);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreadCount; ++t) {
        threads.emplace_back([&ns, &offsets, t]() {
            auto result = ns.resolve_append_target("/a.txt", kPerCallSize);
            ASSERT_TRUE(result.is_ok());
            ASSERT_TRUE(result.value().has_target);
            offsets[t] = result.value().offset;
        });
    }
    for (auto& thread : threads) thread.join();

    std::sort(offsets.begin(), offsets.end());
    for (int t = 0; t < kThreadCount; ++t) {
        EXPECT_EQ(offsets[t], static_cast<uint64_t>(t) * kPerCallSize);
    }
}

TEST(NamespaceManagerTest, CommitAppendWithMatchingExpectedPriorSizeSucceeds) {
    NamespaceManager ns;
    create_file_with_one_block(ns, /*last_block_size=*/100);

    auto target_result = ns.resolve_append_target("/a.txt", 20);
    ASSERT_TRUE(target_result.is_ok());
    const AppendTarget& target = target_result.value();

    ErrorCode result = ns.commit_append(
        "/a.txt", AppendCommitRequest{/*has_existing_target=*/true, target.block_id,
                                       /*expected_prior_size=*/100, target.fill_amount, {},
                                       /*last_new_block_size=*/0, /*total_appended_bytes=*/20});
    EXPECT_EQ(result, ErrorCode::OK);

    auto stat_result = ns.stat_file("/a.txt");
    ASSERT_TRUE(stat_result.is_ok());
    EXPECT_EQ(stat_result.value().size, 120u);

    auto block_result = ns.block_metadata(target.block_id);
    ASSERT_TRUE(block_result.is_ok());
    EXPECT_EQ(block_result.value().size, 120u);
}

TEST(NamespaceManagerTest, CommitAppendWithStaleExpectedPriorSizeTimesOut) {
    // No earlier append will ever land with expected_prior_size=999, so this
    // must time out rather than hang forever or silently move size backward
    // (see docs/design.md §14).
    NamespaceManager ns;
    create_file_with_one_block(ns, /*last_block_size=*/100);

    auto target_result = ns.resolve_append_target("/a.txt", 20);
    ASSERT_TRUE(target_result.is_ok());
    const AppendTarget& target = target_result.value();

    auto start = std::chrono::steady_clock::now();
    ErrorCode result = ns.commit_append(
        "/a.txt", AppendCommitRequest{/*has_existing_target=*/true, target.block_id,
                                       /*expected_prior_size=*/999, target.fill_amount, {},
                                       /*last_new_block_size=*/0, /*total_appended_bytes=*/20});
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(result, ErrorCode::TIMEOUT);
    EXPECT_GE(elapsed, std::chrono::milliseconds(APPEND_COMMIT_WAIT_TIMEOUT_MS));
}

TEST(NamespaceManagerTest, CommitAppendWaitsForEarlierCommitThenSucceeds) {
    // B reserves after A (offset 100..120 vs 120..130) and tries to commit
    // first — it must wait for A's commit (expected_prior_size=100) rather
    // than being rejected outright, since ordinary network/scheduling
    // reordering between two concurrent appends is not an error condition
    // (see docs/design.md §14).
    NamespaceManager ns;
    create_file_with_one_block(ns, /*last_block_size=*/100);

    auto target_a = ns.resolve_append_target("/a.txt", 20);   // offset 100, fill 20
    auto target_b = ns.resolve_append_target("/a.txt", 10);   // offset 120, fill 10
    ASSERT_TRUE(target_a.is_ok());
    ASSERT_TRUE(target_b.is_ok());

    ErrorCode b_result = ErrorCode::INTERNAL_ERROR;
    std::thread b_thread([&]() {
        b_result = ns.commit_append(
            "/a.txt", AppendCommitRequest{true, target_b.value().block_id,
                                           /*expected_prior_size=*/120, target_b.value().fill_amount,
                                           {}, /*last_new_block_size=*/0, /*total_appended_bytes=*/10});
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ErrorCode a_result = ns.commit_append(
        "/a.txt", AppendCommitRequest{true, target_a.value().block_id,
                                       /*expected_prior_size=*/100, target_a.value().fill_amount, {},
                                       /*last_new_block_size=*/0, /*total_appended_bytes=*/20});
    b_thread.join();

    EXPECT_EQ(a_result, ErrorCode::OK);
    EXPECT_EQ(b_result, ErrorCode::OK);

    auto stat_result = ns.stat_file("/a.txt");
    ASSERT_TRUE(stat_result.is_ok());
    EXPECT_EQ(stat_result.value().size, 130u);  // 100 + 20 + 10, not reordered
}

TEST(NamespaceManagerTest, CommitAppendExtendsBlockListWithNewBlocks) {
    NamespaceManager ns;
    create_file_with_one_block(ns, /*last_block_size=*/BLOCK_SIZE);  // last block already full

    auto new_block_result = ns.allocate_block("/a.txt");
    ASSERT_TRUE(new_block_result.is_ok());
    BlockId new_block_id = new_block_result.value();
    ns.set_block_replicas(new_block_id, {1, 2, 3});

    ErrorCode result = ns.commit_append(
        "/a.txt", AppendCommitRequest{/*has_existing_target=*/false, 0, 0, 0, {new_block_id},
                                       /*last_new_block_size=*/30, /*total_appended_bytes=*/30});
    EXPECT_EQ(result, ErrorCode::OK);

    auto stat_result = ns.stat_file("/a.txt");
    ASSERT_TRUE(stat_result.is_ok());
    EXPECT_EQ(stat_result.value().size, BLOCK_SIZE + 30);
    ASSERT_EQ(stat_result.value().blocks.size(), 2u);
    EXPECT_EQ(stat_result.value().blocks.back(), new_block_id);

    // A subsequent append must see this new block's REAL size (30), not 0 —
    // regression test for a bug where a brand-new block's reserved_size was
    // never bumped away from its allocate_block() default, causing the next
    // append to compute offset 0 and silently overwrite it.
    auto new_block_meta = ns.block_metadata(new_block_id);
    ASSERT_TRUE(new_block_meta.is_ok());
    EXPECT_EQ(new_block_meta.value().size, 30u);

    auto next_target = ns.resolve_append_target("/a.txt", 5);
    ASSERT_TRUE(next_target.is_ok());
    ASSERT_TRUE(next_target.value().has_target);
    EXPECT_EQ(next_target.value().block_id, new_block_id);
    EXPECT_EQ(next_target.value().offset, 30u);
}

TEST(NamespaceManagerTest, CommitAppendRejectsNewBlockNotInPendingSet) {
    NamespaceManager ns;
    ASSERT_EQ(ns.create_file("/a.txt"), ErrorCode::OK);

    ErrorCode result = ns.commit_append(
        "/a.txt", AppendCommitRequest{/*has_existing_target=*/false, 0, 0, 0,
                                       {/*never allocated=*/99999}, /*last_new_block_size=*/5,
                                       /*total_appended_bytes=*/5});
    EXPECT_EQ(result, ErrorCode::INVALID_REQUEST);
}

TEST(NamespaceManagerTest, CommitAppendRejectsBlockIdBelongingToAnotherFile) {
    // Regression test: existing_block_id must be verified as path's actual
    // current last block. Without that check, a malformed or adversarial
    // APPEND_COMMIT naming a block that belongs to a DIFFERENT file could
    // silently corrupt that other file's block metadata, since blocks_ is a
    // single flat map shared across every file (see docs/design.md §14).
    NamespaceManager ns;
    BlockId other_files_block = create_file_with_one_block(ns, /*last_block_size=*/0);
    ASSERT_EQ(ns.create_file("/b.txt"), ErrorCode::OK);  // no blocks of its own

    ErrorCode result = ns.commit_append(
        "/b.txt", AppendCommitRequest{/*has_existing_target=*/true, other_files_block,
                                       /*expected_prior_size=*/0, /*filled_amount=*/10, {},
                                       /*last_new_block_size=*/0, /*total_appended_bytes=*/10});
    EXPECT_EQ(result, ErrorCode::INVALID_REQUEST);

    // /a.txt's block must be untouched — still COMMITTED (from
    // create_file_with_one_block's commit_write()) with its original size,
    // not re-committed with the bogus request's values.
    auto block_result = ns.block_metadata(other_files_block);
    ASSERT_TRUE(block_result.is_ok());
    EXPECT_EQ(block_result.value().size, 0u);
    EXPECT_EQ(block_result.value().state, BlockState::COMMITTED);
}

TEST(NamespaceManagerTest, CommitAppendSurvivesConcurrentDeleteFileDuringWait) {
    // Regression test: a CAS wait inside commit_append() unlocks mutex_
    // internally, and used to hold a files_ iterator taken before that
    // wait, dereferencing it again afterward — a use-after-free if a
    // concurrent delete_file() erased the entry in the meantime (see
    // docs/design.md §14). With the fix, commit_append() re-validates the
    // file exists (and re-derives everything else) fresh after any wait,
    // so a delete mid-wait can only ever surface as a clean TIMEOUT here:
    // deleting the file also makes it impossible for any OTHER commit to
    // ever satisfy this call's CAS predicate (that commit's own existence
    // check would fail first), so the dangerous "wake up, then dereference
    // a stale file_it" path is unreachable post-fix — this test exists to
    // run cleanly under ASan/TSan-style tooling and confirm exactly that:
    // no crash, and a timeout rather than any success.
    NamespaceManager ns;
    create_file_with_one_block(ns, /*last_block_size=*/100);

    auto target = ns.resolve_append_target("/a.txt", 20);
    ASSERT_TRUE(target.is_ok());
    ASSERT_TRUE(target.value().has_target);

    std::thread deleter([&ns]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ns.delete_file("/a.txt");
    });

    // expected_prior_size intentionally wrong (0, not 100) so this call
    // waits — long enough for the delete above to land mid-wait — and
    // eventually times out rather than ever matching.
    ErrorCode result = ns.commit_append(
        "/a.txt", AppendCommitRequest{true, target.value().block_id, /*expected_prior_size=*/0,
                                       target.value().fill_amount, {}, /*last_new_block_size=*/0,
                                       /*total_appended_bytes=*/20});
    deleter.join();

    EXPECT_EQ(result, ErrorCode::TIMEOUT);
}

TEST(NamespaceManagerTest, ConcurrentStatAndListDontRace) {
    NamespaceManager ns;
    ASSERT_EQ(ns.create_file("/data/a.txt"), ErrorCode::OK);
    ASSERT_EQ(ns.create_file("/data/b.txt"), ErrorCode::OK);

    constexpr int kThreadCount = 8;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreadCount; ++t) {
        threads.emplace_back([&ns]() {
            for (int i = 0; i < 20; ++i) {
                auto stat_result = ns.stat_file("/data/a.txt");
                EXPECT_TRUE(stat_result.is_ok());

                auto list_result = ns.list_files("/data");
                ASSERT_TRUE(list_result.is_ok());
                EXPECT_EQ(list_result.value().size(), 2u);
            }
        });
    }
    for (auto& thread : threads) thread.join();
}
