#ifndef DFS_STORAGE_BLOCK_HEADER_H_
#define DFS_STORAGE_BLOCK_HEADER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common/errors.h"
#include "common/types.h"

constexpr uint32_t BLOCK_MAGIC = 0x424C4B31;  // "BLK1"
// Faza 12.2: bumped for the new block_version field (see docs/design.md
// §16). Safe to break the on-disk format without a migration path — data/
// is gitignored and always freshly recreated per test/dev run.
constexpr uint16_t BLOCK_HEADER_VERSION = 2;
constexpr size_t BLOCK_HEADER_SIZE = 30;  // 4 + 2 + 8 + 4 + 4 + 8 bytes

struct BlockHeader {
    uint32_t magic;
    uint16_t version;
    BlockId block_id;
    uint32_t data_size;
    uint32_t checksum;
    // Faza 12.2: bumped on every successful commit to this block (WRITE or
    // APPEND) — lets NODE_INVENTORY reconciliation (Faza 12.3) tell a
    // stale replica apart from a current one. 0 has no special meaning
    // here (unlike Master's BlockMetadata.version, which starts at 1 —
    // see docs/design.md §16); a Storage Node just stores whatever value
    // it's given.
    uint64_t block_version;
};

std::array<std::byte, BLOCK_HEADER_SIZE> serialize_block_header(const BlockHeader& header);
Result<BlockHeader> deserialize_block_header(std::span<const std::byte, BLOCK_HEADER_SIZE> bytes);

// Standard CRC-32 (IEEE 802.3 polynomial 0xEDB88320) — shared by
// BlockStorage's own at-rest checksum and, as of Faza 12.2,
// StorageNode::handle_copy_block_data()'s in-transit verification against
// the source's claimed checksum (see docs/design.md §16).
uint32_t compute_block_checksum(const std::vector<uint8_t>& data);

#endif  // DFS_STORAGE_BLOCK_HEADER_H_
