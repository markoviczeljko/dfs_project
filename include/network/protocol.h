#ifndef DFS_NETWORK_PROTOCOL_H_
#define DFS_NETWORK_PROTOCOL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common/config.h"
#include "common/errors.h"

constexpr uint32_t MAGIC = 0x44534631;
constexpr uint16_t PROTOCOL_VERSION = 1;

// §5.17 leaves these conceptual; sized here for control metadata vs a
// full data block plus a small overhead for framing fields. See docs/design.md.
constexpr size_t MAX_CONTROL_MESSAGE_SIZE = 64 * 1024;
constexpr size_t MAX_DATA_MESSAGE_SIZE = BLOCK_SIZE + 4096;

constexpr size_t MESSAGE_HEADER_SIZE = 20;

// Reserves slots for every message named across §5/§6/§7/§8/§9 so the wire
// format doesn't need breaking changes once those flows are implemented.
enum class MessageType : uint16_t {
    // Client <-> Master (control/metadata)
    CREATE_FILE,
    DELETE_FILE,
    READ_FILE,
    WRITE_FILE,
    APPEND_FILE,
    LIST_FILES,
    STAT_FILE,

    // Block control (Master <-> Storage Node / Client <-> Master)
    ALLOCATE_BLOCK,
    DELETE_BLOCK,
    REPLICATE_BLOCK,
    COPY_BLOCK,
    // Faza 12.2: the actual node-to-node block transfer COPY_BLOCK
    // triggers — source Storage Node -> destination Storage Node, once
    // Master's COPY_BLOCK command tells the source who to send to (see
    // docs/design.md §16). Two message types for the two roles, same
    // convention as WRITE_COMMIT vs WRITE_BLOCK.
    COPY_BLOCK_DATA,
    GET_BLOCK_INFO,

    // Client <-> Storage Node (data plane)
    READ_BLOCK,
    WRITE_BLOCK,
    READ_OK,
    NOT_MODIFIED,
    STALE_VERSION,
    WRITE_COMMIT,
    APPEND_COMMIT,

    // Master <-> Storage Node (cluster membership)
    REGISTER_NODE,
    HEARTBEAT,

    // Generic
    ACK,
    ERROR
};

// True for message types that carry block-sized payloads (§5.18 data plane).
bool is_data_message(MessageType type);

struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    MessageType message_type;
    uint64_t request_id;
    uint32_t payload_length;
};

struct Message {
    MessageType type;
    uint64_t request_id;
    std::vector<std::byte> payload;
};

namespace Protocol {

std::array<std::byte, MESSAGE_HEADER_SIZE> serialize_header(const MessageHeader& header);
Result<MessageHeader> deserialize_header(std::span<const std::byte, MESSAGE_HEADER_SIZE> bytes);
std::vector<std::byte> serialize(const Message& message);

}  // namespace Protocol

#endif  // DFS_NETWORK_PROTOCOL_H_
