#ifndef DFS_MASTER_MASTER_H_
#define DFS_MASTER_MASTER_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "common/errors.h"
#include "master/failure_detector.h"
#include "master/metadata_store.h"
#include "master/namespace.h"
#include "master/recovery_manager.h"
#include "master/replication_manager.h"
#include "master/storage_registry.h"
#include "network/network_server.h"
#include "network/protocol.h"
#include "statistics/statistics.h"

// The Master process: owns the namespace (NamespaceManager), the Storage
// Node inventory (StorageRegistry) and replica placement
// (ReplicationManager), node liveness (FailureDetector, Faza 12.1), and a
// NetworkServer (accept-loop, reused verbatim from Faza 5), and wires
// CREATE_FILE/DELETE_FILE/LIST_FILES/STAT_FILE/ALLOCATE_BLOCK/WRITE_COMMIT/
// READ_FILE/REGISTER_NODE/APPEND_FILE/APPEND_COMMIT/HEARTBEAT/NODE_INVENTORY
// requests to them.
// See docs/design.md §13, §14, §16.
class Master {
public:
    struct Config {
        uint16_t port;
        // Faza 12.4: where MetadataStore reads/writes metadata.snapshot —
        // see docs/design.md §16.
        std::filesystem::path metadata_dir;
    };

    static Result<std::unique_ptr<Master>> create(Config config);

    Master(const Master&) = delete;
    Master& operator=(const Master&) = delete;

    ErrorCode start();
    void stop();

    // Faza 14: see docs/design.md §18.
    StatisticsSnapshot statistics_snapshot() const;

private:
    Master(Config config, std::unique_ptr<MetadataStore> metadata_store);

    // Faza 14: thin wrapper around route_message() — same pattern as
    // StorageNode's handle_message()/route_message() split (see
    // docs/design.md §18).
    Message handle_message(const Message& request);
    Message route_message(const Message& request);
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
    Message handle_node_inventory(const Message& request);
    Message handle_get_stats(const Message& request);
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
    // Faza 12.4: saves the whole current namespace/block/node snapshot —
    // called at the tail of every durably-mutating handler
    // (CREATE_FILE/DELETE_FILE/WRITE_COMMIT/APPEND_COMMIT/REGISTER_NODE)
    // and passed as RecoveryManager's on_metadata_changed callback (see
    // docs/design.md §16). A no-op, not a fatal error, if metadata_store_
    // is null (never true in production — only relevant if a future test
    // constructs Master some other way) or save() itself fails: losing
    // durability on ONE mutation must not crash a running Master.
    void persist_metadata();

    Config config_;
    std::unique_ptr<MetadataStore> metadata_store_;
    NamespaceManager namespace_manager_;
    StorageRegistry storage_registry_;
    // Faza 14: declared before replication_manager_/recovery_manager_ —
    // RecoveryManager takes a StatisticsManager& in its constructor, and
    // C++ initializes members in declaration order regardless of the
    // initializer list's order, so this must come first (see
    // docs/design.md §18).
    StatisticsManager statistics_;
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
