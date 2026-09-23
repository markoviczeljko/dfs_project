#ifndef DFS_CLIENT_CLIENT_H_
#define DFS_CLIENT_CLIENT_H_

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "client/cache.h"
#include "common/config.h"
#include "common/errors.h"
#include "common/types.h"
#include "network/connection.h"
#include "network/protocol.h"
#include "statistics/statistics.h"

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

        // Faza 13: max bytes the Client's block cache may hold (see
        // docs/design.md §17). Tests may shrink this to exercise eviction
        // cheaply.
        uint64_t cache_max_bytes = DEFAULT_CLIENT_CACHE_BYTES;
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

    // Faza 14: this Client instance's own operation/error counters (see
    // docs/design.md §18). Does not include cache_stats() — see its own
    // comment for why that's a separate accessor.
    StatisticsSnapshot statistics_snapshot() const;

    // Faza 13's CacheStats intentionally isn't folded into
    // StatisticsSnapshot: statistics/ has no business knowing about
    // client/cache.h's type (would create an upward dependency from a
    // generic, reusable library to a Client-specific one), so the two are
    // exposed separately and merged only by callers that want both (see
    // docs/design.md §18).
    CacheStats cache_stats() const;

private:
    struct ReplicaLocation {
        std::string host;
        uint16_t port;
    };

    Client(Config config, TCPConnection master_connection);

    // Faza 14: every public operation is a thin timing/stats wrapper
    // (see .cpp) around one of these — keeps record_operation() calls out
    // of methods with multiple early-return paths, same pattern this
    // project already uses for read_block()/read_block_locked() in
    // BlockStorage (see docs/design.md §18).
    ErrorCode create_file_impl(const std::string& path);
    ErrorCode write_impl(const std::string& path, const std::vector<uint8_t>& data);
    Result<std::vector<uint8_t>> read_impl(const std::string& path);
    ErrorCode append_impl(const std::string& path, const std::vector<uint8_t>& data);
    ErrorCode delete_file_impl(const std::string& path);
    Result<std::vector<std::string>> list_impl(const std::string& path);
    Result<FileStat> stat_impl(const std::string& path);

    Result<Message> call_master(const Message& request);
    uint64_t next_request_id();
    // Faza 12.2: idempotency token for WRITE_BLOCK, distinct from
    // next_request_id() — a fresh per-Client monotonic counter is NOT
    // safe for this purpose (two different Client processes could both
    // start at 1 and collide on the same Storage Node), so this uses a
    // random 64-bit value instead (see docs/design.md §16).
    OperationId next_operation_id();

    // Parses the [u32 host_len][host][u16 port] x count replica list shared
    // by ALLOCATE_BLOCK's and APPEND_FILE's responses, starting at *offset*
    // (advanced past the list on success).
    Result<std::vector<ReplicaLocation>> parse_replica_list(const std::vector<std::byte>& payload,
                                                             size_t& offset, uint32_t count);

    // Sends one chunk to every listed replica at the given block+offset,
    // one fresh connection per replica. Faza 18: all replicas are written
    // in parallel (fan-out, one thread per replica) and the call waits for
    // every one of them; it returns OK only if all ACKed (strict 3/3),
    // otherwise the first error in replica order.
    ErrorCode write_chunk_to_replicas(BlockId block_id, uint64_t offset, uint64_t block_version,
                                       const uint8_t* data, size_t len,
                                       const std::vector<ReplicaLocation>& replicas);

    // Chunks [data, data+len) into config_.block_size pieces, each a
    // brand-new block: ALLOCATE_BLOCK then write_chunk_to_replicas(offset=0)
    // per chunk. Shared by write() (for the whole file) and append() (for
    // whatever doesn't fit in the file's current last block). Returns the
    // new block ids in order.
    // Faza 18: on failure, every block id this call allocated (including
    // the one whose write failed) has already been handed back to Master
    // via abandon_blocks() before the error is returned.
    Result<std::vector<BlockId>> write_new_blocks(const std::string& path, const uint8_t* data,
                                                   size_t len);

    // Faza 18: best-effort ABANDON_BLOCKS — tells Master a failed WRITE/
    // APPEND attempt gave up on these block ids so it can GC any replicas
    // that were already written. Waits for Master's reply, but its outcome
    // never changes the caller's original error (a failure here just
    // leaves the blocks orphaned, as before this fix).
    void abandon_blocks(const std::string& path, const std::vector<BlockId>& block_ids);

    Config config_;
    TCPConnection master_connection_;
    uint64_t next_request_id_ = 1;
    std::mt19937_64 operation_id_rng_{std::random_device{}()};
    // Faza 13: block-level read cache (see docs/design.md §17).
    ClientCache cache_;
    // Faza 14: see docs/design.md §18.
    StatisticsManager statistics_;
};

#endif  // DFS_CLIENT_CLIENT_H_
