#ifndef DFS_STORAGE_STORAGE_NODE_H_
#define DFS_STORAGE_STORAGE_NODE_H_

#include <cstdint>
#include <filesystem>
#include <memory>

#include "common/errors.h"
#include "common/types.h"
#include "network/network_server.h"
#include "network/protocol.h"
#include "storage/block_manager.h"

// The Storage Node process: owns a BlockManager (local block persistence)
// and a NetworkServer (accept-loop), and wires WRITE_BLOCK/READ_BLOCK/
// DELETE_BLOCK requests to it. Block ids are supplied directly by whatever
// client connects (see docs/design.md §9). Faza 9: registers itself with
// Master at startup (advertise_host/master_host/master_port) so Master can
// choose it as a replica target — see docs/design.md §13.
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
    ErrorCode register_with_master();
    static Message make_error(uint64_t request_id, ErrorCode error);

    Config config_;
    std::unique_ptr<BlockManager> block_manager_;
    std::unique_ptr<NetworkServer> network_server_;
};

#endif  // DFS_STORAGE_STORAGE_NODE_H_
