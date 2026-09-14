#ifndef DFS_STORAGE_BLOCK_HEADER_H_
#define DFS_STORAGE_BLOCK_HEADER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "common/errors.h"
#include "common/types.h"

constexpr uint32_t BLOCK_MAGIC = 0x424C4B31;  // "BLK1"
constexpr uint16_t BLOCK_HEADER_VERSION = 1;
constexpr size_t BLOCK_HEADER_SIZE = 22;  // 4 + 2 + 8 + 4 + 4 bytes

struct BlockHeader {
    uint32_t magic;
    uint16_t version;
    BlockId block_id;
    uint32_t data_size;
    uint32_t checksum;
};

std::array<std::byte, BLOCK_HEADER_SIZE> serialize_block_header(const BlockHeader& header);
Result<BlockHeader> deserialize_block_header(std::span<const std::byte, BLOCK_HEADER_SIZE> bytes);

#endif  // DFS_STORAGE_BLOCK_HEADER_H_
