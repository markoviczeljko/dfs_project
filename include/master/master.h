#ifndef DFS_MASTER_MASTER_H_
#define DFS_MASTER_MASTER_H_

#include <cstdint>
#include <memory>

#include "common/errors.h"
#include "master/namespace.h"
#include "network/network_server.h"
#include "network/protocol.h"

// The Master process: owns the namespace (NamespaceManager) and a
// NetworkServer (accept-loop, reused verbatim from Faza 5), and wires
// CREATE_FILE/DELETE_FILE/LIST_FILES/STAT_FILE requests to it. No Storage
// Node coordination yet — client-facing only (see docs/design.md §10).
class Master {
public:
    struct Config {
        uint16_t port;
    };

    static Result<std::unique_ptr<Master>> create(Config config);

    Master(const Master&) = delete;
    Master& operator=(const Master&) = delete;

    ErrorCode start();
    void stop();

private:
    explicit Master(Config config);

    Message handle_message(const Message& request);
    Message handle_create_file(const Message& request);
    Message handle_delete_file(const Message& request);
    Message handle_list_files(const Message& request);
    Message handle_stat_file(const Message& request);
    static Message make_error(uint64_t request_id, ErrorCode error);

    Config config_;
    NamespaceManager namespace_manager_;
    std::unique_ptr<NetworkServer> network_server_;
};

#endif  // DFS_MASTER_MASTER_H_
