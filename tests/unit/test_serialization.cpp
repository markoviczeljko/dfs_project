#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "network/connection.h"
#include "network/socket.h"

TEST(RecvAll, AssemblesMessageDeliveredInFragments) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket receiver_socket(fds[0]);
    TCPConnection receiver(std::move(receiver_socket));

    const std::string data = "distributed-file-system-fragment-test";
    std::thread sender([sender_fd = fds[1], &data]() {
        for (char c : data) {
            ::send(sender_fd, &c, 1, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ::close(sender_fd);
    });

    std::vector<std::byte> buffer(data.size());
    bool ok = receiver.recvAll(buffer.data(), buffer.size());
    sender.join();

    ASSERT_TRUE(ok);
    EXPECT_EQ(std::memcmp(buffer.data(), data.data(), data.size()), 0);
}

TEST(SendAll, DeliversFullPayloadOverMultipleWrites) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    // Force a small send buffer so a 200 KB payload cannot leave in a
    // single send() call, exercising sendAll()'s retry loop.
    int small_buf = 4096;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small_buf, sizeof(small_buf));

    Socket sender_socket(fds[0]);
    TCPConnection sender(std::move(sender_socket));

    std::vector<std::byte> payload(200000);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i % 256);
    }

    std::vector<std::byte> received(payload.size());
    std::thread reader([receiver_fd = fds[1], &received]() {
        size_t total = 0;
        while (total < received.size()) {
            ssize_t n = ::recv(receiver_fd, received.data() + total, received.size() - total, 0);
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        ::close(receiver_fd);
    });

    bool ok = sender.sendAll(payload.data(), payload.size());
    reader.join();

    ASSERT_TRUE(ok);
    EXPECT_EQ(received, payload);
}
