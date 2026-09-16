#include "network/socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// A send() on a connection the peer already closed raises SIGPIPE, whose
// default disposition kills the whole process — not just this thread. This
// was unreachable before Faza 11 (nothing ever closed a connection out
// from under a still-writing peer); Master's DELETE_BLOCK timeout (see
// docs/design.md §15) makes it reachable: a Storage Node slow enough to
// reply after Master's socket has already timed out and closed would
// otherwise crash on its own ACK send(). Ignoring SIGPIPE process-wide
// once, here, makes that send() fail with EPIPE instead — exactly what
// Socket::send()/TCPConnection::sendAll() already treat as a plain, safe
// failure. Every process that ever creates a Socket (Master, Client,
// StorageNode, and every test binary) goes through this exactly once.
void ignore_sigpipe_once() {
    static std::once_flag flag;
    std::call_once(flag, [] { std::signal(SIGPIPE, SIG_IGN); });
}

}  // namespace

Socket::~Socket() { close_fd(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close_fd();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void Socket::close_fd() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Result<Socket> Socket::create() {
    ignore_sigpipe_once();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return Result<Socket>::err(ErrorCode::INTERNAL_ERROR);
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    return Result<Socket>::ok(Socket(fd));
}

ErrorCode Socket::connect(const std::string& host, uint16_t port) {
    if (!valid()) return ErrorCode::INTERNAL_ERROR;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return ErrorCode::INVALID_REQUEST;
    }

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return errno == ETIMEDOUT ? ErrorCode::TIMEOUT : ErrorCode::INTERNAL_ERROR;
    }
    return ErrorCode::OK;
}

ErrorCode Socket::set_timeout(int timeout_ms) {
    if (!valid()) return ErrorCode::INTERNAL_ERROR;

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        return ErrorCode::INTERNAL_ERROR;
    }
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        return ErrorCode::INTERNAL_ERROR;
    }
    return ErrorCode::OK;
}

ErrorCode Socket::bind(uint16_t port) {
    if (!valid()) return ErrorCode::INTERNAL_ERROR;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return ErrorCode::INTERNAL_ERROR;
    }
    return ErrorCode::OK;
}

ErrorCode Socket::listen(int backlog) {
    if (!valid()) return ErrorCode::INTERNAL_ERROR;
    if (::listen(fd_, backlog) < 0) {
        return ErrorCode::INTERNAL_ERROR;
    }
    return ErrorCode::OK;
}

Result<Socket> Socket::accept() {
    if (!valid()) return Result<Socket>::err(ErrorCode::INTERNAL_ERROR);

    int client_fd = ::accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
        return Result<Socket>::err(ErrorCode::INTERNAL_ERROR);
    }
    return Result<Socket>::ok(Socket(client_fd));
}

ssize_t Socket::send(const std::byte* data, size_t size) {
    return ::send(fd_, data, size, 0);
}

ssize_t Socket::recv(std::byte* buffer, size_t size) {
    return ::recv(fd_, buffer, size, 0);
}
