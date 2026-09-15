#ifndef DFS_NETWORK_NETWORK_SERVER_H_
#define DFS_NETWORK_NETWORK_SERVER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "common/errors.h"
#include "network/protocol.h"
#include "network/socket.h"

// Generic TCP accept-loop shared by any process that speaks the DFS wire
// protocol (Storage Node, later Master). Knows nothing about BlockManager or
// namespaces — just accepts connections and hands every received Message to
// the caller-supplied handler, thread-per-connection (see docs/design.md).
class NetworkServer {
public:
    using MessageHandler = std::function<Message(const Message&)>;

    static Result<std::unique_ptr<NetworkServer>> create(uint16_t port, MessageHandler handler);

    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    ~NetworkServer();

    ErrorCode start();
    void stop();

private:
    NetworkServer(Socket listen_socket, MessageHandler handler);

    void accept_loop();
    void handle_connection(Socket client_socket);

    Socket listen_socket_;
    MessageHandler handler_;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
};

#endif  // DFS_NETWORK_NETWORK_SERVER_H_
