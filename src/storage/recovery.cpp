#include "storage/recovery.h"

#include <endian.h>

#include <cstring>

namespace {

void put_u32(std::vector<std::byte>& buf, uint32_t value) {
    uint32_t be = htobe32(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}

void put_u64(std::vector<std::byte>& buf, uint64_t value) {
    uint64_t be = htobe64(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}

}  // namespace

namespace StorageRecovery {

std::vector<std::byte> build_inventory_payload(StorageNodeId node_id, BlockManager& manager) {
    // Wire format: [u64 node_id][u32 block_count]
    // {[u64 block_id][u64 version][u32 checksum]} x block_count (see
    // docs/design.md §16, Master::handle_node_inventory).
    auto blocks = manager.enumerate_blocks();
    std::vector<std::byte> payload;
    put_u64(payload, node_id);
    put_u32(payload, static_cast<uint32_t>(blocks.size()));
    for (const auto& [block_id, info] : blocks) {
        put_u64(payload, block_id);
        put_u64(payload, info.version);
        put_u32(payload, info.checksum);
    }
    return payload;
}

}  // namespace StorageRecovery
