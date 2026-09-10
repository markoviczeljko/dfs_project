#include "network/socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
