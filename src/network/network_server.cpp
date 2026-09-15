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

    // Every still-open connection's worker thread may be blocked inside
    // recv(). Shut each one down first — otherwise join() below could hang
    // forever on an idle-but-connected client — then join every worker so
    // none is left running against a MessageHandler whose owner is about to
    // be destroyed right after stop() returns.
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto& worker : workers_) {
        ::shutdown(worker.fd, SHUT_RDWR);
    }
    for (auto& worker : workers_) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
    workers_.clear();
}

void NetworkServer::accept_loop() {
    while (true) {
        auto accepted = listen_socket_.accept();
        if (accepted.is_error()) {
            // Either a transient error or stop() shut the listening socket
            // down — either way, this thread's job is done.
            return;
        }

        int fd = accepted.value().fd();
        auto finished = std::make_shared<std::atomic<bool>>(false);
        std::thread worker(&NetworkServer::handle_connection, this, std::move(accepted.value()), finished);

        std::lock_guard<std::mutex> lock(workers_mutex_);
        // Opportunistically reap workers whose connection already closed on
        // its own, so a long-lived server doesn't accumulate joinable
        // thread handles forever across many short-lived connections.
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->finished->load()) {
                if (it->thread.joinable()) {
                    it->thread.join();
                }
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
        workers_.push_back(Worker{std::move(worker), fd, std::move(finished)});
    }
}

void NetworkServer::handle_connection(Socket client_socket, std::shared_ptr<std::atomic<bool>> finished) {
    TCPConnection connection(std::move(client_socket));
    while (true) {
        auto request = connection.receiveMessage();
        if (!request.has_value()) {
            break;  // client disconnected or sent something unframeable
        }
        Message response = handler_(*request);
        if (!connection.sendMessage(response)) {
            break;
        }
    }
    finished->store(true);
}
