#ifndef DFS_MASTER_MASTER_H_
#define DFS_MASTER_MASTER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "common/errors.h"
#include "master/failure_detector.h"
#include "master/namespace.h"
#include "master/recovery_manager.h"
#include "master/replication_manager.h"
#include "master/storage_registry.h"
#include "network/network_server.h"
#include "network/protocol.h"

// The Master process: owns the namespace (NamespaceManager), the Storage
// Node inventory (StorageRegistry) and replica placement
// (ReplicationManager), node liveness (FailureDetector, Faza 12.1), and a
// NetworkServer (accept-loop, reused verbatim from Faza 5), and wires
// CREATE_FILE/DELETE_FILE/LIST_FILES/STAT_FILE/ALLOCATE_BLOCK/WRITE_COMMIT/
// READ_FILE/REGISTER_NODE/APPEND_FILE/APPEND_COMMIT/HEARTBEAT requests to
// them.
// See docs/design.md §13, §14, §16.
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
    Message handle_heartbeat(const Message& request);
    static Message make_error(uint64_t request_id, ErrorCode error);

    // Faza 11: best-effort physical cleanup of blocks DELETE_FILE/WRITE
    // overwrite just dropped from the namespace. Synchronous — the caller's
    // ACK isn't sent until every DELETE_BLOCK attempt has been made — and
    // non-fatal: a node that's unreachable or doesn't ACK is skipped, never
    // turned into an error for the client. See docs/design.md §15.
    void gc_dispatch(const std::vector<OrphanedBlock>& orphans);
    // Opens a fresh connection to node_id and sends DELETE_BLOCK(block_id).
    // Returns true only on an ACK response; false for an unknown node,
    // connect failure, or any non-ACK response.
    bool send_delete_block(StorageNodeId node_id, BlockId block_id);
    uint64_t next_request_id();

    Config config_;
    NamespaceManager namespace_manager_;
    StorageRegistry storage_registry_;
    ReplicationManager replication_manager_;
    // Constructed before failure_detector_ so its callback can reference
    // it; on_metadata_changed stays the default no-op until Faza 12.4
    // wires it to Master's persistence hook.
    RecoveryManager recovery_manager_;
    FailureDetector failure_detector_;
    std::unique_ptr<NetworkServer> network_server_;
    // Atomic: unlike Client (single-threaded/CLI use), Master's NetworkServer
    // is thread-per-connection, so concurrent DELETE_FILE/WRITE_COMMIT
    // requests can call next_request_id() from different threads at once
    // (see docs/design.md §15).
    std::atomic<uint64_t> next_request_id_{1};
};

#endif  // DFS_MASTER_MASTER_H_
