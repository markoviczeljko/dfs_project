#ifndef DFS_NETWORK_CONNECTION_H_
#define DFS_NETWORK_CONNECTION_H_

#include <cstddef>
#include <optional>
#include <utility>

#include "network/protocol.h"
#include "network/socket.h"

// Wraps a connected Socket with message framing on top of raw send/recv.
class TCPConnection {
public:
    explicit TCPConnection(Socket socket) : socket_(std::move(socket)) {}

    bool sendAll(const std::byte* data, size_t size);
    bool recvAll(std::byte* buffer, size_t size);

    bool sendMessage(const Message& message);
    std::optional<Message> receiveMessage();

private:
    Socket socket_;
};

#endif  // DFS_NETWORK_CONNECTION_H_
