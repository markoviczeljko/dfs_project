#include <gtest/gtest.h>

#include <memory>

#include "client/client.h"
#include "master/master.h"

// First integration test spanning two real components (Master + Client) as
// in-process objects over real TCP loopback — see docs/design.md §11 for
// why in-process (not subprocess) was chosen as the test strategy going
// forward for every later integration test.
class CreateIntegrationTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (master_) master_->stop();
    }

    void start_master(uint16_t port) {
        Master::Config config{port, "127.0.0.1", 0};
        auto result = Master::create(config);
        ASSERT_TRUE(result.is_ok());
        master_ = std::move(result.value());
        ASSERT_EQ(master_->start(), ErrorCode::OK);
    }

    static std::unique_ptr<Client> connect_client(uint16_t master_port) {
        auto result = Client::create(Client::Config{"127.0.0.1", master_port});
        EXPECT_TRUE(result.is_ok());
        return std::move(result.value());
    }

    std::unique_ptr<Master> master_;
};

TEST_F(CreateIntegrationTest, CreateSucceedsThenDuplicateFails) {
    start_master(39001);
    auto client = connect_client(39001);

    EXPECT_EQ(client->create_file("/data/test.txt"), ErrorCode::OK);
    EXPECT_EQ(client->create_file("/data/test.txt"), ErrorCode::FILE_ALREADY_EXISTS);
}

TEST_F(CreateIntegrationTest, CreateOnInvalidPathFails) {
    start_master(39002);
    auto client = connect_client(39002);

    EXPECT_EQ(client->create_file("relative.txt"), ErrorCode::INVALID_REQUEST);
}
