#ifndef DFS_CLIENT_CLIENT_H_
#define DFS_CLIENT_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/errors.h"
#include "network/connection.h"
#include "network/protocol.h"

// The Client: talks to Master for namespace/control operations and directly
// to a Storage Node for block data (control/data plane split, see
// docs/design.md §11). write()/read() split/reassemble files spanning
// multiple blocks (see docs/design.md §12). Not thread-safe — one Client is
// meant for single-threaded/CLI use, not shared across threads.
class Client {
public:
    struct Config {
        std::string master_host;
        uint16_t master_port;
        // Chunk size write() splits data into. Defaults to the real
        // BLOCK_SIZE; tests may override it smaller to exercise multi-block
        // logic cheaply without moving real 64MB payloads over loopback TCP.
        // This is purely a Client-side concern — Master/NamespaceManager
        // never need to know what chunk size was used (see docs/design.md
        // §12).
        uint64_t block_size = BLOCK_SIZE;
    };

    static Result<std::unique_ptr<Client>> create(Config config);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ErrorCode create_file(const std::string& path);
    ErrorCode write(const std::string& path, const std::vector<uint8_t>& data);
    Result<std::vector<uint8_t>> read(const std::string& path);

private:
    Client(Config config, TCPConnection master_connection);

    Result<Message> call_master(const Message& request);
    uint64_t next_request_id();

    Config config_;
    TCPConnection master_connection_;
    uint64_t next_request_id_ = 1;
};

#endif  // DFS_CLIENT_CLIENT_H_
