#ifndef DFS_CLIENT_CLIENT_H_
#define DFS_CLIENT_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/errors.h"
#include "network/connection.h"
#include "network/protocol.h"

// The Client: talks to Master for namespace/control operations and directly
// to a Storage Node for block data (control/data plane split, see
// docs/design.md §11). Single-block only in this phase (Faza 8 adds
// multi-block WRITE/READ). Not thread-safe — one Client is meant for
// single-threaded/CLI use, not shared across threads.
class Client {
public:
    struct Config {
        std::string master_host;
        uint16_t master_port;
    };

    static Result<std::unique_ptr<Client>> create(Config config);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ErrorCode create_file(const std::string& path);
    ErrorCode write(const std::string& path, const std::vector<uint8_t>& data);
    Result<std::vector<uint8_t>> read(const std::string& path);

private:
    explicit Client(TCPConnection master_connection);

    Result<Message> call_master(const Message& request);
    uint64_t next_request_id();

    TCPConnection master_connection_;
    uint64_t next_request_id_ = 1;
};

#endif  // DFS_CLIENT_CLIENT_H_
