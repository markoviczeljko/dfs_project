#include "storage/storage_node.h"

#include <endian.h>

#include <cstring>
#include <utility>

namespace {

// Mirrors the private wire-level helpers in protocol.cpp — kept local since
// WRITE_BLOCK/READ_BLOCK/DELETE_BLOCK payload layouts are this phase's own
// invention, not part of the Faza 1 protocol surface (see docs/design.md §9).

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

constexpr size_t kBlockIdSize = sizeof(uint64_t);
constexpr size_t kDataSizeFieldSize = sizeof(uint32_t);

}  // namespace

StorageNode::StorageNode(Config config, std::unique_ptr<BlockManager> block_manager)
    : config_(std::move(config)), block_manager_(std::move(block_manager)) {}

Result<std::unique_ptr<StorageNode>> StorageNode::create(Config config) {
    auto block_manager_result = BlockManager::create(config.data_dir, config.capacity_bytes);
    if (block_manager_result.is_error()) {
        return Result<std::unique_ptr<StorageNode>>::err(block_manager_result.error());
    }

    auto node = std::unique_ptr<StorageNode>(
        new StorageNode(std::move(config), std::move(block_manager_result.value())));
    return Result<std::unique_ptr<StorageNode>>::ok(std::move(node));
}

ErrorCode StorageNode::start() {
    auto server_result = NetworkServer::create(
        config_.port, [this](const Message& request) { return handle_message(request); });
    if (server_result.is_error()) {
        return server_result.error();
    }
    network_server_ = std::move(server_result.value());
    return network_server_->start();
}

void StorageNode::stop() {
    if (network_server_) {
        network_server_->stop();
    }
}

Message StorageNode::make_error(uint64_t request_id, ErrorCode error) {
    std::vector<std::byte> payload;
    put_u16(payload, static_cast<uint16_t>(error));
    return Message{MessageType::ERROR, request_id, std::move(payload)};
}

Message StorageNode::handle_message(const Message& request) {
    switch (request.type) {
        case MessageType::WRITE_BLOCK:
            return handle_write_block(request);
        case MessageType::READ_BLOCK:
            return handle_read_block(request);
        case MessageType::DELETE_BLOCK:
            return handle_delete_block(request);
        default:
            return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
}

Message StorageNode::handle_write_block(const Message& request) {
    if (request.payload.size() < kBlockIdSize + kDataSizeFieldSize) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    BlockId block_id = get_u64(request.payload.data());
    uint32_t data_size = get_u32(request.payload.data() + kBlockIdSize);
    size_t expected_size = kBlockIdSize + kDataSizeFieldSize + data_size;
    if (request.payload.size() != expected_size) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    std::vector<uint8_t> data(data_size);
    if (data_size > 0) {
        std::memcpy(data.data(), request.payload.data() + kBlockIdSize + kDataSizeFieldSize, data_size);
    }

    ErrorCode result = block_manager_->write_block(block_id, data);
    if (result != ErrorCode::OK) {
        return make_error(request.request_id, result);
    }

    std::vector<std::byte> response_payload;
    put_u64(response_payload, block_id);
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}

Message StorageNode::handle_read_block(const Message& request) {
    if (request.payload.size() != kBlockIdSize) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    BlockId block_id = get_u64(request.payload.data());
    auto result = block_manager_->read_block(block_id);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }

    const std::vector<uint8_t>& data = result.value();
    std::vector<std::byte> response_payload;
    put_u64(response_payload, block_id);
    put_u32(response_payload, static_cast<uint32_t>(data.size()));
    const auto* data_bytes = reinterpret_cast<const std::byte*>(data.data());
    response_payload.insert(response_payload.end(), data_bytes, data_bytes + data.size());

    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}

Message StorageNode::handle_delete_block(const Message& request) {
    if (request.payload.size() != kBlockIdSize) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    BlockId block_id = get_u64(request.payload.data());
    block_manager_->release_block(block_id);  // idempotent — see docs/design.md §7

    std::vector<std::byte> response_payload;
    put_u64(response_payload, block_id);
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}
