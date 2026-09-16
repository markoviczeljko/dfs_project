#include "storage/storage_node.h"

#include <endian.h>

#include <chrono>
#include <cstring>
#include <utility>

#include "common/config.h"
#include "network/connection.h"
#include "network/socket.h"
#include "storage/block_header.h"
#include "storage/recovery.h"

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

constexpr size_t kBlockIdSize = sizeof(uint64_t);
constexpr size_t kOffsetSize = sizeof(uint64_t);
constexpr size_t kDataSizeFieldSize = sizeof(uint32_t);
constexpr size_t kOperationIdSize = sizeof(uint64_t);
constexpr size_t kBlockVersionSize = sizeof(uint64_t);
constexpr size_t kChecksumSize = sizeof(uint32_t);
constexpr size_t kNodeIdSize = sizeof(uint64_t);
constexpr size_t kPortSize = sizeof(uint16_t);

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
    if (ErrorCode err = network_server_->start(); err != ErrorCode::OK) {
        return err;
    }
    // Register with Master only after this node is actually reachable, so
    // it can never be picked as a replica target before it's ready to
    // serve WRITE_BLOCK/READ_BLOCK requests (see docs/design.md §13).
    if (ErrorCode err = register_with_master(); err != ErrorCode::OK) {
        return err;
    }
    // Faza 12.3: reports this node's actual on-disk contents right after
    // registering — same gate on the return value as registration, since a
    // node Master can't reconcile is no more useful than one it can't
    // register (see docs/design.md §16).
    if (ErrorCode err = send_inventory(); err != ErrorCode::OK) {
        return err;
    }

    heartbeat_running_ = true;
    heartbeat_thread_ = std::thread(&StorageNode::heartbeat_loop, this);
    return ErrorCode::OK;
}

ErrorCode StorageNode::register_with_master() {
    auto socket_result = Socket::create();
    if (socket_result.is_error()) {
        return socket_result.error();
    }
    Socket socket = std::move(socket_result.value());
    if (ErrorCode err = socket.connect(config_.master_host, config_.master_port); err != ErrorCode::OK) {
        return err;
    }
    TCPConnection master_connection(std::move(socket));

    // Wire format: [u64 node_id][u32 host_len][host][u16 port]
    // [u64 capacity_bytes] (see docs/design.md §13).
    std::vector<std::byte> payload;
    put_u64(payload, config_.node_id);
    put_string(payload, config_.advertise_host);
    put_u16(payload, config_.port);
    put_u64(payload, config_.capacity_bytes);

    if (!master_connection.sendMessage(Message{MessageType::REGISTER_NODE, 1, std::move(payload)})) {
        return ErrorCode::INTERNAL_ERROR;
    }
    auto response = master_connection.receiveMessage();
    if (!response.has_value()) {
        return ErrorCode::INTERNAL_ERROR;
    }
    if (response->type != MessageType::ACK) {
        if (response->payload.size() < sizeof(uint16_t)) {
            return ErrorCode::INTERNAL_ERROR;
        }
        return static_cast<ErrorCode>(get_u16(response->payload.data()));
    }
    return ErrorCode::OK;
}

ErrorCode StorageNode::send_inventory() {
    auto socket_result = Socket::create();
    if (socket_result.is_error()) {
        return socket_result.error();
    }
    Socket socket = std::move(socket_result.value());
    if (ErrorCode err = socket.connect(config_.master_host, config_.master_port); err != ErrorCode::OK) {
        return err;
    }
    TCPConnection master_connection(std::move(socket));

    std::vector<std::byte> payload =
        StorageRecovery::build_inventory_payload(config_.node_id, *block_manager_);
    if (!master_connection.sendMessage(Message{MessageType::NODE_INVENTORY, 1, std::move(payload)})) {
        return ErrorCode::INTERNAL_ERROR;
    }
    auto response = master_connection.receiveMessage();
    if (!response.has_value() || response->type != MessageType::ACK) {
        return ErrorCode::INTERNAL_ERROR;
    }
    return ErrorCode::OK;
}

void StorageNode::stop() {
    if (heartbeat_running_.exchange(false)) {
        heartbeat_cv_.notify_all();
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
    }
    if (network_server_) {
        network_server_->stop();
    }
}

void StorageNode::heartbeat_loop() {
    std::unique_lock lock(heartbeat_cv_mutex_);
    while (heartbeat_running_.load()) {
        // Matches FailureDetector::sweep_loop()'s idiom: wait_for returns
        // immediately on stop()'s notify_all() rather than blocking out a
        // full HEARTBEAT_INTERVAL_SECONDS.
        heartbeat_cv_.wait_for(lock, std::chrono::seconds(HEARTBEAT_INTERVAL_SECONDS),
                                [this] { return !heartbeat_running_.load(); });
        if (!heartbeat_running_.load()) break;
        lock.unlock();

        auto socket_result = Socket::create();
        if (socket_result.is_ok()) {
            Socket socket = std::move(socket_result.value());
            if (socket.set_timeout(HEARTBEAT_SEND_TIMEOUT_MS) == ErrorCode::OK &&
                socket.connect(config_.master_host, config_.master_port) == ErrorCode::OK) {
                TCPConnection master_connection(std::move(socket));
                std::vector<std::byte> payload;
                put_u64(payload, config_.node_id);
                master_connection.sendMessage(Message{MessageType::HEARTBEAT, 1, std::move(payload)});
                auto response = master_connection.receiveMessage();
                // Faza 12.3: needs_inventory==1 means Master lost track of
                // this node's contents (e.g. after a Faza 12.4 restart) and
                // needs a fresh report — best-effort, like everything else
                // in this loop; a failure here just means the next tick's
                // heartbeat sees needs_inventory again (see docs/design.md
                // §16).
                if (response.has_value() && response->type == MessageType::ACK &&
                    !response->payload.empty() &&
                    static_cast<uint8_t>(response->payload[0]) == 1) {
                    send_inventory();
                }
            }
            // Any failure here (create/timeout/connect/send/recv) is
            // silently ignored — the next tick simply tries again. See
            // docs/design.md §16.
        }

        lock.lock();
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
        case MessageType::COPY_BLOCK:
            return handle_copy_block(request);
        case MessageType::COPY_BLOCK_DATA:
            return handle_copy_block_data(request);
        default:
            return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
}

Message StorageNode::handle_write_block(const Message& request) {
    // Faza 12.2 wire format: [u64 operation_id][u64 block_id][u64 offset]
    // [u64 block_version][u32 data_size][data] (see docs/design.md §16;
    // extends Faza 10's format with operation_id/block_version, prepended/
    // inserted respectively). offset == 0 keeps write_block()'s existing
    // full-overwrite behavior unchanged; offset > 0 is the append-at-offset
    // path (see docs/design.md §14).
    size_t header_size =
        kOperationIdSize + kBlockIdSize + kOffsetSize + kBlockVersionSize + kDataSizeFieldSize;
    if (request.payload.size() < header_size) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    size_t cursor = 0;
    OperationId operation_id = get_u64(request.payload.data() + cursor);
    cursor += kOperationIdSize;
    BlockId block_id = get_u64(request.payload.data() + cursor);
    cursor += kBlockIdSize;
    uint64_t offset = get_u64(request.payload.data() + cursor);
    cursor += kOffsetSize;
    uint64_t block_version = get_u64(request.payload.data() + cursor);
    cursor += kBlockVersionSize;
    uint32_t data_size = get_u32(request.payload.data() + cursor);
    cursor += kDataSizeFieldSize;
    if (request.payload.size() != cursor + data_size) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    // Idempotency: a retried WRITE_BLOCK (lost ACK) must not double-apply —
    // see docs/design.md §16, §8.13/§8.18-§8.20.
    auto existing = operation_log_.begin_or_get(operation_id);
    if (existing.has_value()) {
        if (existing->state == OperationState::IN_PROGRESS) {
            // A genuinely concurrent duplicate (not the common lost-ACK
            // case, which would already be COMPLETED/FAILED by the time a
            // retry arrives) — reject rather than block; the caller's own
            // bounded retry policy will try again (see docs/design.md §16).
            return make_error(request.request_id, ErrorCode::TIMEOUT);
        }
        MessageType type =
            (existing->result == ErrorCode::OK) ? MessageType::ACK : MessageType::ERROR;
        return Message{type, request.request_id, existing->response_payload};
    }

    std::vector<uint8_t> data(data_size);
    if (data_size > 0) {
        std::memcpy(data.data(), request.payload.data() + cursor, data_size);
    }

    // offset == 0 always means "this is the block's complete, definitive
    // content" (full overwrite/truncate), never "append at offset 0 into an
    // already-nonempty block": Master never hands out offset 0 as an append
    // target for an existing block, since a block only becomes an append
    // target once resolve_append_target() has been called against it at
    // least once, and any block's very first append is by construction the
    // first thing to ever write to it beyond allocate_block() (see
    // docs/design.md §14). If that invariant ever changes, this branch
    // needs a real is_new_block flag instead of overloading offset==0.
    ErrorCode result = (offset == 0)
                            ? block_manager_->write_block(block_id, data, block_version)
                            : block_manager_->write_block_at(block_id, offset, data, block_version);

    std::vector<std::byte> response_payload;
    if (result == ErrorCode::OK) {
        put_u64(response_payload, block_id);
    } else {
        put_u16(response_payload, static_cast<uint16_t>(result));
    }
    operation_log_.complete(operation_id, result, response_payload);

    if (result != ErrorCode::OK) {
        return Message{MessageType::ERROR, request.request_id, std::move(response_payload)};
    }
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

Message StorageNode::handle_copy_block(const Message& request) {
    // Source role. Wire format: [u64 operation_id][u64 block_id]
    // [u64 expected_version][u32 dest_host_len][dest_host][u16 dest_port]
    // [u64 dest_node_id] (dest_node_id is carried for logging/error context
    // only — the connection itself is addressed by host/port) (see
    // docs/design.md §16).
    //
    // No operation_log_ dedup here: a retried COPY_BLOCK just re-sends the
    // same COPY_BLOCK_DATA, which IS deduped (shared log) on the
    // destination — end-to-end idempotency doesn't need this leg to be
    // idempotent on its own too.
    size_t header_size = kOperationIdSize + kBlockIdSize + kBlockVersionSize + sizeof(uint32_t);
    if (request.payload.size() < header_size) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    size_t cursor = 0;
    OperationId operation_id = get_u64(request.payload.data() + cursor);
    cursor += kOperationIdSize;
    BlockId block_id = get_u64(request.payload.data() + cursor);
    cursor += kBlockIdSize;
    uint64_t expected_version = get_u64(request.payload.data() + cursor);
    cursor += kBlockVersionSize;
    uint32_t dest_host_len = get_u32(request.payload.data() + cursor);
    cursor += sizeof(uint32_t);
    if (request.payload.size() != cursor + dest_host_len + kPortSize + kNodeIdSize) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    std::string dest_host(reinterpret_cast<const char*>(request.payload.data() + cursor),
                           dest_host_len);
    cursor += dest_host_len;
    uint16_t dest_port = get_u16(request.payload.data() + cursor);
    cursor += kPortSize;
    // dest_node_id trailing field intentionally unread — see comment above.

    auto info = block_manager_->get_block_info(block_id);
    if (info.is_error()) {
        return make_error(request.request_id, info.error());
    }
    // Defensive: Master picked this node as a known-healthy source, so a
    // mismatch here means something already went wrong locally — refuse
    // rather than propagate bad data to a third node.
    if (info.value().version != expected_version) {
        return make_error(request.request_id, ErrorCode::BLOCK_CORRUPTED);
    }
    auto data_result = block_manager_->read_block(block_id);
    if (data_result.is_error()) {
        return make_error(request.request_id, data_result.error());
    }
    const std::vector<uint8_t>& data = data_result.value();

    auto socket_result = Socket::create();
    if (socket_result.is_error()) {
        return make_error(request.request_id, socket_result.error());
    }
    Socket socket = std::move(socket_result.value());
    if (ErrorCode err = socket.set_timeout(COPY_BLOCK_TIMEOUT_MS); err != ErrorCode::OK) {
        return make_error(request.request_id, err);
    }
    if (ErrorCode err = socket.connect(dest_host, dest_port); err != ErrorCode::OK) {
        return make_error(request.request_id, err);
    }
    TCPConnection dest_connection(std::move(socket));

    std::vector<std::byte> copy_payload;
    put_u64(copy_payload, operation_id);
    put_u64(copy_payload, block_id);
    put_u64(copy_payload, expected_version);
    put_u32(copy_payload, static_cast<uint32_t>(data.size()));
    const auto* data_bytes = reinterpret_cast<const std::byte*>(data.data());
    copy_payload.insert(copy_payload.end(), data_bytes, data_bytes + data.size());
    put_u32(copy_payload, compute_block_checksum(data));

    if (!dest_connection.sendMessage(
            Message{MessageType::COPY_BLOCK_DATA, operation_id, std::move(copy_payload)})) {
        return make_error(request.request_id, ErrorCode::INTERNAL_ERROR);
    }
    auto dest_response = dest_connection.receiveMessage();
    if (!dest_response.has_value()) {
        return make_error(request.request_id, ErrorCode::INTERNAL_ERROR);
    }
    if (dest_response->type != MessageType::ACK) {
        if (dest_response->payload.size() < sizeof(uint16_t)) {
            return make_error(request.request_id, ErrorCode::INTERNAL_ERROR);
        }
        return make_error(request.request_id,
                           static_cast<ErrorCode>(get_u16(dest_response->payload.data())));
    }
    return Message{MessageType::ACK, request.request_id, {}};
}

Message StorageNode::handle_copy_block_data(const Message& request) {
    // Destination role. Wire format: [u64 operation_id][u64 block_id]
    // [u64 block_version][u32 data_size][data][u32 source_checksum] (see
    // docs/design.md §16, §8.14 — verify before ever writing to disk).
    size_t fixed_size = kOperationIdSize + kBlockIdSize + kBlockVersionSize + kDataSizeFieldSize;
    if (request.payload.size() < fixed_size + kChecksumSize) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    size_t cursor = 0;
    OperationId operation_id = get_u64(request.payload.data() + cursor);
    cursor += kOperationIdSize;
    BlockId block_id = get_u64(request.payload.data() + cursor);
    cursor += kBlockIdSize;
    uint64_t block_version = get_u64(request.payload.data() + cursor);
    cursor += kBlockVersionSize;
    uint32_t data_size = get_u32(request.payload.data() + cursor);
    cursor += kDataSizeFieldSize;
    if (request.payload.size() != cursor + data_size + kChecksumSize) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    // Shared with WRITE_BLOCK's dedup — see operation_log.h.
    auto existing = operation_log_.begin_or_get(operation_id);
    if (existing.has_value()) {
        if (existing->state == OperationState::IN_PROGRESS) {
            return make_error(request.request_id, ErrorCode::TIMEOUT);
        }
        MessageType type =
            (existing->result == ErrorCode::OK) ? MessageType::ACK : MessageType::ERROR;
        return Message{type, request.request_id, existing->response_payload};
    }

    std::vector<uint8_t> data(data_size);
    if (data_size > 0) {
        std::memcpy(data.data(), request.payload.data() + cursor, data_size);
    }
    cursor += data_size;
    uint32_t source_checksum = get_u32(request.payload.data() + cursor);

    ErrorCode result;
    if (compute_block_checksum(data) != source_checksum) {
        // In-transit corruption — never write it, even provisionally (see
        // docs/design.md §16, §8.14).
        result = ErrorCode::BLOCK_CORRUPTED;
    } else {
        result = block_manager_->write_block(block_id, data, block_version);
    }

    std::vector<std::byte> response_payload;
    if (result == ErrorCode::OK) {
        put_u64(response_payload, block_id);
    } else {
        put_u16(response_payload, static_cast<uint16_t>(result));
    }
    operation_log_.complete(operation_id, result, response_payload);

    if (result != ErrorCode::OK) {
        return Message{MessageType::ERROR, request.request_id, std::move(response_payload)};
    }
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}
