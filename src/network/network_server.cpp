#include "network/network_server.h"

#include <sys/socket.h>

#include <utility>

#include "network/connection.h"

namespace {
constexpr int kListenBacklog = 16;
}  // namespace

NetworkServer::NetworkServer(Socket listen_socket, MessageHandler handler)
    : listen_socket_(std::move(listen_socket)), handler_(std::move(handler)) {}

NetworkServer::~NetworkServer() { stop(); }

Result<std::unique_ptr<NetworkServer>> NetworkServer::create(uint16_t port, MessageHandler handler) {
    auto socket_result = Socket::create();
    if (socket_result.is_error()) {
        return Result<std::unique_ptr<NetworkServer>>::err(socket_result.error());
    }

    Socket socket = std::move(socket_result.value());
    if (ErrorCode err = socket.bind(port); err != ErrorCode::OK) {
        return Result<std::unique_ptr<NetworkServer>>::err(err);
    }
    if (ErrorCode err = socket.listen(kListenBacklog); err != ErrorCode::OK) {
        return Result<std::unique_ptr<NetworkServer>>::err(err);
    }

    auto server = std::unique_ptr<NetworkServer>(new NetworkServer(std::move(socket), std::move(handler)));
    return Result<std::unique_ptr<NetworkServer>>::ok(std::move(server));
}

ErrorCode NetworkServer::start() {
    if (running_.exchange(true)) {
        return ErrorCode::OK;  // already running
    }
    accept_thread_ = std::thread(&NetworkServer::accept_loop, this);
    return ErrorCode::OK;
}

void NetworkServer::stop() {
    if (!running_.exchange(false)) {
        return;  // already stopped
    }
    // Unblocks the accept-thread's blocking accept() call.
    ::shutdown(listen_socket_.fd(), SHUT_RDWR);
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void NetworkServer::accept_loop() {
    while (true) {
        auto accepted = listen_socket_.accept();
        if (accepted.is_error()) {
            // Either a transient error or stop() shut the listening socket
            // down — either way, this thread's job is done.
            return;
        }
        std::thread(&NetworkServer::handle_connection, this, std::move(accepted.value())).detach();
    }
}

void NetworkServer::handle_connection(Socket client_socket) {
    TCPConnection connection(std::move(client_socket));
    while (true) {
        auto request = connection.receiveMessage();
        if (!request.has_value()) {
            return;  // client disconnected or sent something unframeable
        }
        Message response = handler_(*request);
        if (!connection.sendMessage(response)) {
            return;
        }
    }
}
