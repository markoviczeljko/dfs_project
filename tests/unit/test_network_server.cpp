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

TEST(NetworkServer, StopClosesLiveConnectionWithoutHangingOrLeakingTheThread) {
    // Regression test for a heap-use-after-free: stop() used to only join
    // the accept thread, leaving any per-connection worker thread (detached,
    // possibly still blocked in recv() on a client that never disconnects)
    // running against a MessageHandler whose owner could be destroyed right
    // after stop() returned. stop() must now shut every live connection down
    // and join its worker before returning.
    constexpr uint16_t port = 35005;
    auto server = make_echo_server(port);
    ASSERT_EQ(server->start(), ErrorCode::OK);

    // Client connects but never sends anything — its worker thread is
    // parked in receiveMessage()/recv() when stop() is called below.
    TCPConnection client = connect_client(port);

    // If stop() left the worker thread dangling, this either hangs (thread
    // stuck forever) or races with a freed handler; if fixed, it returns
    // promptly with every worker joined.
    server->stop();

    // The server-side socket must now be closed — sending on it fails
    // instead of silently vanishing into a dead thread.
    std::vector<std::byte> payload{std::byte{1}};
    client.sendMessage(Message{MessageType::HEARTBEAT, 1, payload});
    EXPECT_FALSE(client.receiveMessage().has_value());
}
