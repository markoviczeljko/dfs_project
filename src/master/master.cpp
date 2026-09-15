#include "master/master.h"

#include <endian.h>

#include <cstring>
#include <utility>

namespace {

// Mirrors the ad hoc wire-level helpers in storage_node.cpp — kept local
// since CREATE_FILE/DELETE_FILE/LIST_FILES/STAT_FILE payload layouts are
// this phase's own invention, not part of the Faza 1 protocol surface
// (see docs/design.md §10).

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

std::string bytes_to_string(const std::vector<std::byte>& payload) {
    if (payload.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
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

Master::Master(Config config) : config_(std::move(config)) {}

Result<std::unique_ptr<Master>> Master::create(Config config) {
    auto master = std::unique_ptr<Master>(new Master(std::move(config)));
    return Result<std::unique_ptr<Master>>::ok(std::move(master));
}

ErrorCode Master::start() {
    auto server_result = NetworkServer::create(
        config_.port, [this](const Message& request) { return handle_message(request); });
    if (server_result.is_error()) {
        return server_result.error();
    }
    network_server_ = std::move(server_result.value());
    return network_server_->start();
}

void Master::stop() {
    if (network_server_) {
        network_server_->stop();
    }
}

Message Master::make_error(uint64_t request_id, ErrorCode error) {
    std::vector<std::byte> payload;
    put_u16(payload, static_cast<uint16_t>(error));
    return Message{MessageType::ERROR, request_id, std::move(payload)};
}

Message Master::handle_message(const Message& request) {
    switch (request.type) {
        case MessageType::CREATE_FILE:
            return handle_create_file(request);
        case MessageType::DELETE_FILE:
            return handle_delete_file(request);
        case MessageType::LIST_FILES:
            return handle_list_files(request);
        case MessageType::STAT_FILE:
            return handle_stat_file(request);
        case MessageType::ALLOCATE_BLOCK:
            return handle_allocate_block(request);
        case MessageType::WRITE_COMMIT:
            return handle_write_commit(request);
        case MessageType::READ_FILE:
            // Repurposed in Faza 7 as "get file metadata for read" — the
            // design notes' GET_FILE_METADATA was never added to the real
            // enum, so this previously-unused type takes on that role
            // instead (see docs/design.md §11).
            return handle_read_file(request);
        default:
            // Includes WRITE_FILE — never given a defined wire flow
            // anywhere in the design notes, out of scope for this phase
            // (see docs/design.md §10/§11).
            return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
}

Message Master::handle_create_file(const Message& request) {
    std::string path = bytes_to_string(request.payload);
    ErrorCode result = namespace_manager_.create_file(path);
    if (result != ErrorCode::OK) {
        return make_error(request.request_id, result);
    }
    return Message{MessageType::ACK, request.request_id, {}};
}

Message Master::handle_delete_file(const Message& request) {
    std::string path = bytes_to_string(request.payload);
    ErrorCode result = namespace_manager_.delete_file(path);
    if (result != ErrorCode::OK) {
        return make_error(request.request_id, result);
    }
    return Message{MessageType::ACK, request.request_id, {}};
}

Message Master::handle_list_files(const Message& request) {
    std::string path = bytes_to_string(request.payload);
    auto result = namespace_manager_.list_files(path);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }

    const std::vector<std::string>& entries = result.value();
    std::vector<std::byte> response_payload;
    put_u32(response_payload, static_cast<uint32_t>(entries.size()));
    for (const std::string& entry : entries) {
        put_string(response_payload, entry);
    }
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}

Message Master::handle_stat_file(const Message& request) {
    std::string path = bytes_to_string(request.payload);
    auto result = namespace_manager_.stat_file(path);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }

    const FileMetadata& metadata = result.value();
    std::vector<std::byte> response_payload;
    put_u64(response_payload, metadata.id);
    put_u64(response_payload, metadata.size);
    put_u64(response_payload, metadata.modification_time);
    put_u32(response_payload, static_cast<uint32_t>(metadata.blocks.size()));
    for (BlockId block_id : metadata.blocks) {
        put_u64(response_payload, block_id);
    }
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}

Message Master::handle_allocate_block(const Message& request) {
    std::string path = bytes_to_string(request.payload);
    auto result = namespace_manager_.allocate_block(path);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }

    std::vector<std::byte> response_payload;
    put_u64(response_payload, result.value());
    put_string(response_payload, config_.storage_node_host);
    put_u16(response_payload, config_.storage_node_port);
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}

Message Master::handle_write_commit(const Message& request) {
    if (request.payload.size() < sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    uint32_t path_length = get_u32(request.payload.data());
    size_t expected_size = sizeof(uint32_t) + path_length + sizeof(uint64_t) + sizeof(uint64_t);
    if (request.payload.size() != expected_size) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    std::string path(reinterpret_cast<const char*>(request.payload.data() + sizeof(uint32_t)),
                      path_length);
    BlockId block_id = get_u64(request.payload.data() + sizeof(uint32_t) + path_length);
    uint64_t size =
        get_u64(request.payload.data() + sizeof(uint32_t) + path_length + sizeof(uint64_t));

    ErrorCode result = namespace_manager_.commit_write(path, block_id, size);
    if (result != ErrorCode::OK) {
        return make_error(request.request_id, result);
    }
    return Message{MessageType::ACK, request.request_id, {}};
}

Message Master::handle_read_file(const Message& request) {
    std::string path = bytes_to_string(request.payload);
    auto result = namespace_manager_.stat_file(path);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }

    const FileMetadata& metadata = result.value();
    std::vector<std::byte> response_payload;
    put_u64(response_payload, metadata.id);
    put_u64(response_payload, metadata.size);
    put_u64(response_payload, metadata.modification_time);
    put_u32(response_payload, static_cast<uint32_t>(metadata.blocks.size()));
    for (BlockId block_id : metadata.blocks) {
        put_u64(response_payload, block_id);
        put_string(response_payload, config_.storage_node_host);
        put_u16(response_payload, config_.storage_node_port);
    }
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}
