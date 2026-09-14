#include <gtest/gtest.h>

#include <algorithm>
#include <thread>
#include <vector>

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
