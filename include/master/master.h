#ifndef DFS_MASTER_MASTER_H_
#define DFS_MASTER_MASTER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "common/errors.h"
#include "master/namespace.h"
#include "master/replication_manager.h"
#include "master/storage_registry.h"
#include "network/network_server.h"
#include "network/protocol.h"

// The Master process: owns the namespace (NamespaceManager), the Storage
// Node inventory (StorageRegistry) and replica placement
// (ReplicationManager), and a NetworkServer (accept-loop, reused verbatim
// from Faza 5), and wires CREATE_FILE/DELETE_FILE/LIST_FILES/STAT_FILE/
// ALLOCATE_BLOCK/WRITE_COMMIT/READ_FILE/REGISTER_NODE/APPEND_FILE/
// APPEND_COMMIT requests to them.
// See docs/design.md §13, §14.
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
    Message handle_allocate_block(const Message& request);
    Message handle_write_commit(const Message& request);
    Message handle_read_file(const Message& request);
    Message handle_register_node(const Message& request);
    Message handle_append_file(const Message& request);
    Message handle_append_commit(const Message& request);
    static Message make_error(uint64_t request_id, ErrorCode error);

    Config config_;
    NamespaceManager namespace_manager_;
    StorageRegistry storage_registry_;
    ReplicationManager replication_manager_;
    std::unique_ptr<NetworkServer> network_server_;
};

#endif  // DFS_MASTER_MASTER_H_
