#include "client/client.h"

#include <endian.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "common/config.h"
#include "common/types.h"
#include "network/socket.h"

namespace {

// Same ad hoc wire-level helper convention as master.cpp/storage_node.cpp —
// each phase/component encodes its own payloads locally, protocol.h/cpp is
// never touched (see docs/design.md §11).

void put_u16(std::vector<std::byte>& buf, uint16_t value) {
    uint16_t be = htobe16(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&be);
    buf.insert(buf.end(), bytes, bytes + sizeof(be));
}

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

void put_string(std::vector<std::byte>& buf, const std::string& value) {
    put_u32(buf, static_cast<uint32_t>(value.size()));
    const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
    buf.insert(buf.end(), bytes, bytes + value.size());
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

std::vector<std::byte> string_to_bytes(const std::string& value) {
    std::vector<std::byte> bytes;
    const auto* data = reinterpret_cast<const std::byte*>(value.data());
    bytes.insert(bytes.end(), data, data + value.size());
    return bytes;
}

ErrorCode error_from_payload(const std::vector<std::byte>& payload) {
    if (payload.size() < sizeof(uint16_t)) {
        return ErrorCode::INTERNAL_ERROR;
    }
    return static_cast<ErrorCode>(get_u16(payload.data()));
}

}  // namespace

Client::Client(Config config, TCPConnection master_connection)
    : config_(std::move(config)), master_connection_(std::move(master_connection)) {}

Result<std::unique_ptr<Client>> Client::create(Config config) {
    auto socket_result = Socket::create();
    if (socket_result.is_error()) {
        return Result<std::unique_ptr<Client>>::err(socket_result.error());
    }

    Socket socket = std::move(socket_result.value());
    if (ErrorCode err = socket.connect(config.master_host, config.master_port); err != ErrorCode::OK) {
        return Result<std::unique_ptr<Client>>::err(err);
    }

    auto client =
        std::unique_ptr<Client>(new Client(std::move(config), TCPConnection(std::move(socket))));
    return Result<std::unique_ptr<Client>>::ok(std::move(client));
}

uint64_t Client::next_request_id() { return next_request_id_++; }

Result<Message> Client::call_master(const Message& request) {
    if (!master_connection_.sendMessage(request)) {
        return Result<Message>::err(ErrorCode::INTERNAL_ERROR);
    }
    auto response = master_connection_.receiveMessage();
    if (!response.has_value()) {
        return Result<Message>::err(ErrorCode::INTERNAL_ERROR);
    }
    return Result<Message>::ok(std::move(*response));
}

ErrorCode Client::create_file(const std::string& path) {
    Message request{MessageType::CREATE_FILE, next_request_id(), string_to_bytes(path)};
    auto result = call_master(request);
    if (result.is_error()) {
        return result.error();
    }
    const Message& response = result.value();
    if (response.type == MessageType::ACK) {
        return ErrorCode::OK;
    }
    return error_from_payload(response.payload);
}

ErrorCode Client::write(const std::string& path, const std::vector<uint8_t>& data) {
    // block_size must be positive and never exceed the real BLOCK_SIZE — a
    // larger chunk would exceed MAX_DATA_MESSAGE_SIZE at the network layer
    // and fail with a confusing INTERNAL_ERROR deep inside TCPConnection
    // instead of a clean INVALID_REQUEST here.
    if (config_.block_size == 0 || config_.block_size > BLOCK_SIZE) {
        return ErrorCode::INVALID_REQUEST;
    }

    size_t total = data.size();
    size_t chunk_count = total == 0 ? 0 : (total + config_.block_size - 1) / config_.block_size;
    std::vector<BlockId> block_ids;
    block_ids.reserve(chunk_count);

    for (size_t i = 0; i < chunk_count; ++i) {
        size_t offset = i * config_.block_size;
        size_t len = std::min<size_t>(config_.block_size, total - offset);

        // Step 1: ask Master for a BlockId + target Storage Node.
        Message alloc_request{MessageType::ALLOCATE_BLOCK, next_request_id(), string_to_bytes(path)};
        auto alloc_result = call_master(alloc_request);
        if (alloc_result.is_error()) {
            return alloc_result.error();
        }
        const Message& alloc_response = alloc_result.value();
        if (alloc_response.type != MessageType::ACK) {
            return error_from_payload(alloc_response.payload);
        }
        if (alloc_response.payload.size() < sizeof(uint64_t) + sizeof(uint32_t)) {
            return ErrorCode::INTERNAL_ERROR;
        }
        BlockId block_id = get_u64(alloc_response.payload.data());
        uint32_t host_length = get_u32(alloc_response.payload.data() + sizeof(uint64_t));
        size_t host_offset = sizeof(uint64_t) + sizeof(uint32_t);
        if (alloc_response.payload.size() != host_offset + host_length + sizeof(uint16_t)) {
            return ErrorCode::INTERNAL_ERROR;
        }
        std::string host(reinterpret_cast<const char*>(alloc_response.payload.data() + host_offset),
                          host_length);
        uint16_t port = get_u16(alloc_response.payload.data() + host_offset + host_length);

        // Step 2: write this chunk directly to the Storage Node (data
        // plane — never routed through Master). A fresh connection per
        // chunk, consistent with the existing no-pooling decision (see
        // docs/design.md §11).
        auto socket_result = Socket::create();
        if (socket_result.is_error()) {
            return socket_result.error();
        }
        Socket socket = std::move(socket_result.value());
        if (ErrorCode err = socket.connect(host, port); err != ErrorCode::OK) {
            return err;
        }
        TCPConnection node_connection(std::move(socket));

        std::vector<std::byte> write_payload;
        put_u64(write_payload, block_id);
        put_u32(write_payload, static_cast<uint32_t>(len));
        const auto* data_bytes = reinterpret_cast<const std::byte*>(data.data() + offset);
        write_payload.insert(write_payload.end(), data_bytes, data_bytes + len);

        if (!node_connection.sendMessage(
                Message{MessageType::WRITE_BLOCK, next_request_id(), std::move(write_payload)})) {
            return ErrorCode::INTERNAL_ERROR;
        }
        auto write_response = node_connection.receiveMessage();
        if (!write_response.has_value()) {
            return ErrorCode::INTERNAL_ERROR;
        }
        if (write_response->type != MessageType::ACK) {
            // Abort without committing — the old file version stays intact
            // (see docs/design.md §12); already-allocated/written blocks
            // for this attempt become orphans (GC deferred).
            return error_from_payload(write_response->payload);
        }

        block_ids.push_back(block_id);
    }

    // Step 3: tell Master the whole ordered block list is committed, so it
    // becomes visible via STAT_FILE/READ_FILE in one atomic step.
    std::vector<std::byte> commit_payload;
    put_string(commit_payload, path);
    put_u64(commit_payload, static_cast<uint64_t>(total));
    put_u32(commit_payload, static_cast<uint32_t>(block_ids.size()));
    for (BlockId block_id : block_ids) {
        put_u64(commit_payload, block_id);
    }

    Message commit_request{MessageType::WRITE_COMMIT, next_request_id(), std::move(commit_payload)};
    auto commit_result = call_master(commit_request);
    if (commit_result.is_error()) {
        return commit_result.error();
    }
    if (commit_result.value().type != MessageType::ACK) {
        return error_from_payload(commit_result.value().payload);
    }
    return ErrorCode::OK;
}

Result<std::vector<uint8_t>> Client::read(const std::string& path) {
    // Step 1: ask Master where the file's block(s) live.
    Message meta_request{MessageType::READ_FILE, next_request_id(), string_to_bytes(path)};
    auto meta_result = call_master(meta_request);
    if (meta_result.is_error()) {
        return Result<std::vector<uint8_t>>::err(meta_result.error());
    }
    const Message& meta_response = meta_result.value();
    if (meta_response.type != MessageType::ACK) {
        return Result<std::vector<uint8_t>>::err(error_from_payload(meta_response.payload));
    }
    if (meta_response.payload.size() < 3 * sizeof(uint64_t) + sizeof(uint32_t)) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
    }

    uint64_t total_size = get_u64(meta_response.payload.data() + sizeof(uint64_t));
    uint32_t block_count = get_u32(meta_response.payload.data() + 3 * sizeof(uint64_t));
    if (block_count == 0) {
        return Result<std::vector<uint8_t>>::ok({});  // empty file — nothing written yet
    }

    // Parse every (block_id, host, port) tuple, in order — the metadata
    // response was already N-block-ready before this phase (see
    // docs/design.md §12).
    struct BlockLocation {
        BlockId block_id;
        std::string host;
        uint16_t port;
    };
    std::vector<BlockLocation> locations;
    locations.reserve(block_count);
    size_t offset = 3 * sizeof(uint64_t) + sizeof(uint32_t);
    for (uint32_t i = 0; i < block_count; ++i) {
        if (meta_response.payload.size() < offset + sizeof(uint64_t) + sizeof(uint32_t)) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
        }
        BlockId block_id = get_u64(meta_response.payload.data() + offset);
        offset += sizeof(uint64_t);
        uint32_t host_length = get_u32(meta_response.payload.data() + offset);
        offset += sizeof(uint32_t);
        if (meta_response.payload.size() < offset + host_length + sizeof(uint16_t)) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
        }
        std::string host(reinterpret_cast<const char*>(meta_response.payload.data() + offset),
                          host_length);
        offset += host_length;
        uint16_t port = get_u16(meta_response.payload.data() + offset);
        offset += sizeof(uint16_t);
        locations.push_back(BlockLocation{block_id, std::move(host), port});
    }
    if (meta_response.payload.size() != offset) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
    }

    // Step 2: read every block directly from its Storage Node, in order,
    // and concatenate. A fresh connection per block, consistent with the
    // existing no-pooling decision (see docs/design.md §11).
    std::vector<uint8_t> result;
    result.reserve(total_size);
    for (const BlockLocation& location : locations) {
        auto socket_result = Socket::create();
        if (socket_result.is_error()) {
            return Result<std::vector<uint8_t>>::err(socket_result.error());
        }
        Socket socket = std::move(socket_result.value());
        if (ErrorCode err = socket.connect(location.host, location.port); err != ErrorCode::OK) {
            return Result<std::vector<uint8_t>>::err(err);
        }
        TCPConnection node_connection(std::move(socket));

        std::vector<std::byte> read_payload;
        put_u64(read_payload, location.block_id);
        if (!node_connection.sendMessage(
                Message{MessageType::READ_BLOCK, next_request_id(), std::move(read_payload)})) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
        }
        auto read_response = node_connection.receiveMessage();
        if (!read_response.has_value()) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
        }
        if (read_response->type != MessageType::ACK) {
            return Result<std::vector<uint8_t>>::err(error_from_payload(read_response->payload));
        }
        if (read_response->payload.size() < sizeof(uint64_t) + sizeof(uint32_t)) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
        }

        uint32_t data_size = get_u32(read_response->payload.data() + sizeof(uint64_t));
        size_t data_offset = sizeof(uint64_t) + sizeof(uint32_t);
        if (read_response->payload.size() != data_offset + data_size) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
        }

        const auto* block_bytes = read_response->payload.data() + data_offset;
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(block_bytes),
                       reinterpret_cast<const uint8_t*>(block_bytes) + data_size);
    }
    return Result<std::vector<uint8_t>>::ok(std::move(result));
}
