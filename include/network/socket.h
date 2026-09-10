#ifndef DFS_NETWORK_SOCKET_H_
#define DFS_NETWORK_SOCKET_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

#include "common/errors.h"

// Move-only RAII wrapper around a POSIX socket file descriptor.
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    static Result<Socket> create();

    ErrorCode connect(const std::string& host, uint16_t port);
    ErrorCode bind(uint16_t port);
    ErrorCode listen(int backlog);
    Result<Socket> accept();

    ssize_t send(const std::byte* data, size_t size);
    ssize_t recv(std::byte* buffer, size_t size);

    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

private:
    void close_fd();

    int fd_ = -1;
};

#endif  // DFS_NETWORK_SOCKET_H_
