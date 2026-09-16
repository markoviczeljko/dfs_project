#ifndef DFS_STORAGE_STORAGE_NODE_H_
#define DFS_STORAGE_STORAGE_NODE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "common/errors.h"
#include "common/types.h"
#include "network/network_server.h"
#include "network/protocol.h"
#include "storage/block_manager.h"
#include "storage/operation_log.h"

// The Storage Node process: owns a BlockManager (local block persistence)
// and a NetworkServer (accept-loop), and wires WRITE_BLOCK/READ_BLOCK/
// DELETE_BLOCK requests to it. Block ids are supplied directly by whatever
// client connects (see docs/design.md §9). Faza 9: registers itself with
// Master at startup (advertise_host/master_host/master_port) so Master can
// choose it as a replica target — see docs/design.md §13. Faza 12.1: sends
// a periodic HEARTBEAT to Master so it can be told apart from a dead node
// — see docs/design.md §16.
class StorageNode {
public:
    struct Config {
        StorageNodeId node_id;
        uint16_t port;
        std::filesystem::path data_dir;
        uint64_t capacity_bytes;
        std::string advertise_host;
        std::string master_host;
        uint16_t master_port;
    };

    static Result<std::unique_ptr<StorageNode>> create(Config config);

    StorageNode(const StorageNode&) = delete;
    StorageNode& operator=(const StorageNode&) = delete;

    ErrorCode start();
    void stop();

private:
    StorageNode(Config config, std::unique_ptr<BlockManager> block_manager);

    Message handle_message(const Message& request);
    Message handle_write_block(const Message& request);
    Message handle_read_block(const Message& request);
    Message handle_delete_block(const Message& request);
    // Faza 12.2: source role — relays this node's copy of a block to
    // another node, on Master's instruction (see docs/design.md §16).
    Message handle_copy_block(const Message& request);
    // Faza 12.2: destination role — receives a block pushed by
    // handle_copy_block() on another node, verifies its checksum before
    // ever writing to disk.
    Message handle_copy_block_data(const Message& request);
    ErrorCode register_with_master();
    // Faza 12.3: sends this node's full on-disk block inventory to Master
    // on a fresh connection — called once after a successful
    // register_with_master() in start(), and again reactively from
    // heartbeat_loop() whenever a HEARTBEAT ACK's needs_inventory bit is 1
    // (see docs/design.md §16).
    ErrorCode send_inventory();
    static Message make_error(uint64_t request_id, ErrorCode error);

    // Faza 12.1: sends HEARTBEAT{node_id} to Master every
    // HEARTBEAT_INTERVAL_SECONDS on a fresh connection bounded by
    // HEARTBEAT_SEND_TIMEOUT_MS, until stop() fires. A single failed send
    // is logged/ignored, not retried within the same tick — the next tick
    // will simply try again (see docs/design.md §16).
    void heartbeat_loop();

    Config config_;
    std::unique_ptr<BlockManager> block_manager_;
    std::unique_ptr<NetworkServer> network_server_;
    // Faza 12.2: shared by WRITE_BLOCK and COPY_BLOCK_DATA — see
    // operation_log.h for why one shared log is safe (see docs/design.md
    // §16).
    OperationLog operation_log_;

    std::thread heartbeat_thread_;
    std::mutex heartbeat_cv_mutex_;
    std::condition_variable heartbeat_cv_;
    std::atomic<bool> heartbeat_running_{false};
};

#endif  // DFS_STORAGE_STORAGE_NODE_H_
