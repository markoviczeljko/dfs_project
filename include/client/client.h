#ifndef DFS_CLIENT_CLIENT_H_
#define DFS_CLIENT_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/errors.h"
#include "common/types.h"
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

    // Faza 10: appends data to the end of path. See docs/design.md §14.
    ErrorCode append(const std::string& path, const std::vector<uint8_t>& data);

    // Faza 11: removes path and, best-effort, the physical blocks it owned
    // from every replica. See docs/design.md §15.
    ErrorCode delete_file(const std::string& path);

    // Faza 11: immediate children of path (one level, not recursive — see
    // docs/design.md §12/§15).
    Result<std::vector<std::string>> list(const std::string& path);

    struct FileStat {
        FileId id;
        uint64_t size;
        uint64_t modification_time;
        std::vector<BlockId> blocks;
    };
    Result<FileStat> stat(const std::string& path);

private:
    struct ReplicaLocation {
        std::string host;
        uint16_t port;
    };

    Client(Config config, TCPConnection master_connection);

    Result<Message> call_master(const Message& request);
    uint64_t next_request_id();

    // Parses the [u32 host_len][host][u16 port] x count replica list shared
    // by ALLOCATE_BLOCK's and APPEND_FILE's responses, starting at *offset*
    // (advanced past the list on success).
    Result<std::vector<ReplicaLocation>> parse_replica_list(const std::vector<std::byte>& payload,
                                                             size_t& offset, uint32_t count);

    // Sends one chunk to every listed replica at the given block+offset,
    // one fresh connection per replica; strict fail-fast on the first
    // non-ACK (same 3/3 semantics as the original write() loop).
    ErrorCode write_chunk_to_replicas(BlockId block_id, uint64_t offset, const uint8_t* data,
                                       size_t len, const std::vector<ReplicaLocation>& replicas);

    // Chunks [data, data+len) into config_.block_size pieces, each a
    // brand-new block: ALLOCATE_BLOCK then write_chunk_to_replicas(offset=0)
    // per chunk. Shared by write() (for the whole file) and append() (for
    // whatever doesn't fit in the file's current last block). Returns the
    // new block ids in order.
    Result<std::vector<BlockId>> write_new_blocks(const std::string& path, const uint8_t* data,
                                                   size_t len);

    Config config_;
    TCPConnection master_connection_;
    uint64_t next_request_id_ = 1;
};

#endif  // DFS_CLIENT_CLIENT_H_
