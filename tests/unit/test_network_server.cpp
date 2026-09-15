#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "network/connection.h"
#include "network/network_server.h"
#include "network/socket.h"

namespace {

std::unique_ptr<NetworkServer> make_echo_server(uint16_t port) {
    auto result = NetworkServer::create(port, [](const Message& request) {
        return Message{MessageType::ACK, request.request_id, request.payload};
    });
    EXPECT_TRUE(result.is_ok());
    return std::move(result.value());
}

TCPConnection connect_client(uint16_t port) {
    auto socket_result = Socket::create();
    EXPECT_TRUE(socket_result.is_ok());
    Socket socket = std::move(socket_result.value());
    EXPECT_EQ(socket.connect("127.0.0.1", port), ErrorCode::OK);
    return TCPConnection(std::move(socket));
}

}  // namespace

TEST(NetworkServer, EchoesSingleRequest) {
    constexpr uint16_t port = 35001;
    auto server = make_echo_server(port);
    ASSERT_EQ(server->start(), ErrorCode::OK);

    TCPConnection client = connect_client(port);
    std::vector<std::byte> payload{std::byte{9}, std::byte{8}, std::byte{7}};
    ASSERT_TRUE(client.sendMessage(Message{MessageType::HEARTBEAT, 55, payload}));

    auto response = client.receiveMessage();
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, MessageType::ACK);
    EXPECT_EQ(response->request_id, 55u);
    EXPECT_EQ(response->payload, payload);

    server->stop();
}

TEST(NetworkServer, HandlesMultipleConnectionsConcurrently) {
    constexpr uint16_t port = 35002;
    auto server = make_echo_server(port);
    ASSERT_EQ(server->start(), ErrorCode::OK);

    constexpr int kClients = 5;
    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([port, i]() {
            TCPConnection client = connect_client(port);
            std::vector<std::byte> payload{static_cast<std::byte>(i)};
            ASSERT_TRUE(client.sendMessage(Message{MessageType::HEARTBEAT, static_cast<uint64_t>(i), payload}));
            auto response = client.receiveMessage();
            ASSERT_TRUE(response.has_value());
            EXPECT_EQ(response->request_id, static_cast<uint64_t>(i));
            EXPECT_EQ(response->payload, payload);
        });
    }
    for (auto& t : clients) {
        t.join();
    }

    server->stop();
}

TEST(NetworkServer, StopUnblocksAcceptLoopWithoutHanging) {
    constexpr uint16_t port = 35003;
    auto server = make_echo_server(port);
    ASSERT_EQ(server->start(), ErrorCode::OK);

    // If shutdown()/join() inside stop() didn't unblock the accept thread,
    // this call would hang and the test would time out.
    server->stop();
    SUCCEED();
}

TEST(NetworkServer, StopIsIdempotent) {
    constexpr uint16_t port = 35004;
    auto server = make_echo_server(port);
    ASSERT_EQ(server->start(), ErrorCode::OK);

    server->stop();
    server->stop();  // must not hang or double-join
    SUCCEED();
}
