#include "network/protocol.h"

#include <endian.h>

#include <cstring>

namespace {

void put_u16(std::byte* dest, uint16_t value) {
    uint16_t be = htobe16(value);
    std::memcpy(dest, &be, sizeof(be));
}

void put_u32(std::byte* dest, uint32_t value) {
    uint32_t be = htobe32(value);
    std::memcpy(dest, &be, sizeof(be));
}

void put_u64(std::byte* dest, uint64_t value) {
    uint64_t be = htobe64(value);
    std::memcpy(dest, &be, sizeof(be));
}

uint16_t get_u16(const std::byte* src) {
    uint16_t be;
    std::memcpy(&be, src, sizeof(be));
    return be16toh(be);
}

uint32_t get_u32(const std::byte* src) {
    uint32_t be;
    std::memcpy(&be, src, sizeof(be));
    return be32toh(be);
}

uint64_t get_u64(const std::byte* src) {
    uint64_t be;
    std::memcpy(&be, src, sizeof(be));
    return be64toh(be);
}

}  // namespace

bool is_data_message(MessageType type) {
    switch (type) {
        case MessageType::READ_BLOCK:
        case MessageType::WRITE_BLOCK:
        case MessageType::READ_OK:
        case MessageType::NOT_MODIFIED:
        case MessageType::STALE_VERSION:
        case MessageType::WRITE_COMMIT:
        case MessageType::APPEND_COMMIT:
        case MessageType::COPY_BLOCK:
        // ACK/ERROR carry the reply to whatever request triggered them —
        // in particular a READ_BLOCK's ACK response carries up to one
        // block's worth of data (see docs/design.md §12). The receiver has
        // no way to know which request a given ACK/ERROR answers, so they
        // must always be allowed the generous data-message size limit
        // rather than the small control-message one.
        case MessageType::ACK:
        case MessageType::ERROR:
            return true;
        default:
            return false;
    }
}

namespace Protocol {

std::array<std::byte, MESSAGE_HEADER_SIZE> serialize_header(const MessageHeader& header) {
    std::array<std::byte, MESSAGE_HEADER_SIZE> bytes{};
    put_u32(bytes.data(), header.magic);
    put_u16(bytes.data() + 4, header.version);
    put_u16(bytes.data() + 6, static_cast<uint16_t>(header.message_type));
    put_u64(bytes.data() + 8, header.request_id);
    put_u32(bytes.data() + 16, header.payload_length);
    return bytes;
}

Result<MessageHeader> deserialize_header(std::span<const std::byte, MESSAGE_HEADER_SIZE> bytes) {
    MessageHeader header{};
    header.magic = get_u32(bytes.data());
    header.version = get_u16(bytes.data() + 4);
    header.message_type = static_cast<MessageType>(get_u16(bytes.data() + 6));
    header.request_id = get_u64(bytes.data() + 8);
    header.payload_length = get_u32(bytes.data() + 16);

    if (header.magic != MAGIC || header.version != PROTOCOL_VERSION) {
        return Result<MessageHeader>::err(ErrorCode::INVALID_MESSAGE);
    }
    return Result<MessageHeader>::ok(header);
}

std::vector<std::byte> serialize(const Message& message) {
    MessageHeader header{MAGIC, PROTOCOL_VERSION, message.type, message.request_id,
                          static_cast<uint32_t>(message.payload.size())};
    auto header_bytes = serialize_header(header);

    std::vector<std::byte> bytes;
    bytes.reserve(header_bytes.size() + message.payload.size());
    bytes.insert(bytes.end(), header_bytes.begin(), header_bytes.end());
    bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
    return bytes;
}

}  // namespace Protocol
