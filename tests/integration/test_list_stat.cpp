#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "client/client.h"
#include "common/config.h"
#include "master/master.h"
#include "storage/storage_node.h"

// Faza 11 — Client-side LIST/STAT (see docs/design.md §15). Same cluster
// shape as AppendIntegrationTest/DeleteIntegrationTest.
class ListStatIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("dfs_list_stat_integration_test_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        clients_.clear();
        for (auto& node : storage_nodes_) node->stop();
        if (master_) master_->stop();
        std::filesystem::remove_all(dir_);
    }

    Client& start_cluster(uint16_t master_port, uint16_t storage_port1, uint16_t storage_port2,
                           uint16_t storage_port3, uint64_t block_size = BLOCK_SIZE) {
        Master::Config master_config{master_port};
        auto master_result = Master::create(master_config);
        EXPECT_TRUE(master_result.is_ok());
        master_ = std::move(master_result.value());
        EXPECT_EQ(master_->start(), ErrorCode::OK);
        master_port_ = master_port;

        std::array<uint16_t, 3> storage_ports = {storage_port1, storage_port2, storage_port3};
        for (size_t i = 0; i < storage_ports.size(); ++i) {
            StorageNode::Config storage_config{static_cast<StorageNodeId>(i + 1),
                                                storage_ports[i],
                                                dir_ / ("node" + std::to_string(i + 1)),
                                                1024ULL * 1024 * 1024,
                                                "127.0.0.1",
                                                "127.0.0.1",
                                                master_port};
            auto storage_result = StorageNode::create(storage_config);
            EXPECT_TRUE(storage_result.is_ok());
            storage_nodes_.push_back(std::move(storage_result.value()));
            EXPECT_EQ(storage_nodes_.back()->start(), ErrorCode::OK);
        }

        return new_client(block_size);
    }

    Client& new_client(uint64_t block_size = BLOCK_SIZE) {
        auto client_result = Client::create(Client::Config{"127.0.0.1", master_port_, block_size});
        EXPECT_TRUE(client_result.is_ok());
        clients_.push_back(std::move(client_result.value()));
        return *clients_.back();
    }

    std::filesystem::path dir_;
    std::vector<std::unique_ptr<StorageNode>> storage_nodes_;
    std::unique_ptr<Master> master_;
    std::vector<std::unique_ptr<Client>> clients_;
    uint16_t master_port_ = 0;
};

TEST_F(ListStatIntegrationTest, ListAndStatReflectCreateWriteAppend) {
    Client& client = start_cluster(40500, 40501, 40502, 40503);

    ASSERT_EQ(client.create_file("/data/a.txt"), ErrorCode::OK);
    std::vector<uint8_t> data(10, 1);
    ASSERT_EQ(client.write("/data/a.txt", data), ErrorCode::OK);
    std::vector<uint8_t> more(5, 2);
    ASSERT_EQ(client.append("/data/a.txt", more), ErrorCode::OK);

    auto list_result = client.list("/data");
    ASSERT_TRUE(list_result.is_ok());
    ASSERT_EQ(list_result.value().size(), 1u);
    EXPECT_EQ(list_result.value()[0], "a.txt");

    auto stat_result = client.stat("/data/a.txt");
    ASSERT_TRUE(stat_result.is_ok());
    EXPECT_EQ(stat_result.value().size, 15u);
    EXPECT_EQ(stat_result.value().blocks.size(), 1u);
    EXPECT_GT(stat_result.value().id, 0u);
}

TEST_F(ListStatIntegrationTest, StatAfterDeleteFails) {
    Client& client = start_cluster(40510, 40511, 40512, 40513);

    ASSERT_EQ(client.create_file("/a.txt"), ErrorCode::OK);
    ASSERT_EQ(client.delete_file("/a.txt"), ErrorCode::OK);

    auto stat_result = client.stat("/a.txt");
    ASSERT_TRUE(stat_result.is_error());
    EXPECT_EQ(stat_result.error(), ErrorCode::FILE_NOT_FOUND);
}

TEST_F(ListStatIntegrationTest, ListNoLongerShowsDeletedFile) {
    Client& client = start_cluster(40520, 40521, 40522, 40523);

    ASSERT_EQ(client.create_file("/data/a.txt"), ErrorCode::OK);
    ASSERT_EQ(client.create_file("/data/b.txt"), ErrorCode::OK);
    ASSERT_EQ(client.delete_file("/data/a.txt"), ErrorCode::OK);

    auto list_result = client.list("/data");
    ASSERT_TRUE(list_result.is_ok());
    ASSERT_EQ(list_result.value().size(), 1u);
    EXPECT_EQ(list_result.value()[0], "b.txt");
}

TEST_F(ListStatIntegrationTest, ConcurrentCreateAndListBasicCorrectness) {
    // Basic correctness only — full concurrency stress is explicitly
    // deferred beyond this phase (see docs/design.md §15).
    Client& client = start_cluster(40530, 40531, 40532, 40533);
    ASSERT_EQ(client.create_file("/data/sentinel.txt"), ErrorCode::OK);

    // Both Client objects are created on the main thread before spawning —
    // clients_ is an unsynchronized std::vector, so push_back()ing into it
    // concurrently from two threads would itself be a data race independent
    // of anything being tested here.
    Client& creator_client = new_client();
    Client& lister_client = new_client();

    constexpr int kFileCount = 20;
    std::atomic<int> created{0};
    std::thread creator([&creator_client, &created]() {
        for (int i = 0; i < kFileCount; ++i) {
            ErrorCode result =
                creator_client.create_file("/data/f" + std::to_string(i) + ".txt");
            if (result == ErrorCode::OK) ++created;
        }
    });

    std::atomic<bool> saw_torn_name{false};
    std::thread lister([&lister_client, &saw_torn_name]() {
        for (int i = 0; i < 50; ++i) {
            auto list_result = lister_client.list("/data");
            if (!list_result.is_ok()) continue;
            for (const std::string& name : list_result.value()) {
                bool is_sentinel = name == "sentinel.txt";
                bool is_created_file = name.size() >= 6 && name.front() == 'f' &&
                                        name.compare(name.size() - 4, 4, ".txt") == 0;
                if (!is_sentinel && !is_created_file) saw_torn_name = true;
            }
        }
    });

    creator.join();
    lister.join();

    EXPECT_FALSE(saw_torn_name);
    EXPECT_EQ(created.load(), kFileCount);

    auto final_list = client.list("/data");
    ASSERT_TRUE(final_list.is_ok());
    EXPECT_EQ(final_list.value().size(), static_cast<size_t>(kFileCount + 1));
}

TEST_F(ListStatIntegrationTest, ConcurrentDeleteAndListBasicCorrectness) {
    // Basic correctness only — full concurrency stress is explicitly
    // deferred beyond this phase (see docs/design.md §15).
    Client& client = start_cluster(40540, 40541, 40542, 40543);

    constexpr int kFileCount = 20;
    for (int i = 0; i < kFileCount; ++i) {
        ASSERT_EQ(client.create_file("/data/f" + std::to_string(i) + ".txt"), ErrorCode::OK);
    }

    // See the comment in ConcurrentCreateAndListBasicCorrectness above —
    // both Clients are created on the main thread before spawning.
    Client& deleter_client = new_client();
    Client& lister_client = new_client();

    std::atomic<int> deleted{0};
    std::thread deleter([&deleter_client, &deleted]() {
        for (int i = 0; i < kFileCount; ++i) {
            ErrorCode result = deleter_client.delete_file("/data/f" + std::to_string(i) + ".txt");
            if (result == ErrorCode::OK) ++deleted;
        }
    });

    std::atomic<bool> list_call_failed{false};
    std::thread lister([&lister_client, &list_call_failed]() {
        for (int i = 0; i < 50; ++i) {
            auto list_result = lister_client.list("/data");
            if (!list_result.is_ok()) list_call_failed = true;
        }
    });

    deleter.join();
    lister.join();

    EXPECT_FALSE(list_call_failed);
    EXPECT_EQ(deleted.load(), kFileCount);

    auto final_list = client.list("/data");
    ASSERT_TRUE(final_list.is_ok());
    EXPECT_TRUE(final_list.value().empty());
}
