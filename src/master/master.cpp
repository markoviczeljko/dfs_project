#include "master/master.h"

#include <endian.h>

#include <cstring>
#include <utility>

#include "common/config.h"
#include "network/connection.h"
#include "network/socket.h"

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

Master::Master(Config config)
    : config_(std::move(config)), replication_manager_(storage_registry_) {}

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
        case MessageType::REGISTER_NODE:
            return handle_register_node(request);
        case MessageType::APPEND_FILE:
            return handle_append_file(request);
        case MessageType::APPEND_COMMIT:
            return handle_append_commit(request);
        default:
            // HEARTBEAT stays unhandled in this phase — heartbeat
            // sending/timeout detection is Faza 12's job (see
            // docs/design.md §13).
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
    auto result = namespace_manager_.delete_file(path);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }
    gc_dispatch(result.value());
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
    BlockId block_id = result.value();

    // Faza 9: pick DEFAULT_REPLICATION_FACTOR distinct alive nodes for this
    // block. Fewer than that currently alive/registered is treated as
    // NODE_NOT_FOUND rather than allocating a permanently under-replicated
    // block this phase can never repair (see docs/design.md §13).
    std::vector<StorageNodeId> replicas = replication_manager_.choose_replicas(block_id);
    if (replicas.empty()) {
        // block_id is already sitting in NamespaceManager's pending_blocks_
        // set for this path at this point — same orphan-until-a-later-
        // successful-commit-or-delete fate as an abandoned write attempt
        // (see docs/design.md §12), not a new leak this phase introduces.
        return make_error(request.request_id, ErrorCode::NODE_NOT_FOUND);
    }
    namespace_manager_.set_block_replicas(block_id, replicas);

    // Wire format: [u64 block_id][u32 replica_count]
    // {[u32 host_len][host][u16 port]} x replica_count.
    std::vector<std::byte> response_payload;
    put_u64(response_payload, block_id);
    put_u32(response_payload, static_cast<uint32_t>(replicas.size()));
    for (StorageNodeId node_id : replicas) {
        auto node = storage_registry_.get_node(node_id);
        if (!node.has_value()) {
            return make_error(request.request_id, ErrorCode::INTERNAL_ERROR);
        }
        put_string(response_payload, node->host);
        put_u16(response_payload, node->port);
    }
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}

Message Master::handle_write_commit(const Message& request) {
    // Faza 8 wire format: [u32 path_len][path][u64 size][u32 block_count]
    // [block_count x u64 block_id][u64 last_block_size] — replaces Faza 7's
    // single (block_id, size) pair with an ordered, variable-length block
    // list (see docs/design.md §12). Faza 10 appends last_block_size so
    // Master can track how full the last block actually is, which append
    // needs to compute an offset (see docs/design.md §14).
    if (request.payload.size() < sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    uint32_t path_length = get_u32(request.payload.data());
    size_t offset = sizeof(uint32_t);
    if (request.payload.size() < offset + path_length + sizeof(uint64_t) + sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    std::string path(reinterpret_cast<const char*>(request.payload.data() + offset), path_length);
    offset += path_length;
    uint64_t size = get_u64(request.payload.data() + offset);
    offset += sizeof(uint64_t);
    uint32_t block_count = get_u32(request.payload.data() + offset);
    offset += sizeof(uint32_t);

    size_t expected_size =
        offset + static_cast<size_t>(block_count) * sizeof(uint64_t) + sizeof(uint64_t);
    if (request.payload.size() != expected_size) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    std::vector<BlockId> block_ids;
    block_ids.reserve(block_count);
    for (uint32_t i = 0; i < block_count; ++i) {
        block_ids.push_back(get_u64(request.payload.data() + offset));
        offset += sizeof(uint64_t);
    }
    uint64_t last_block_size = get_u64(request.payload.data() + offset);

    auto result = namespace_manager_.commit_write(path, block_ids, size, last_block_size);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }
    gc_dispatch(result.value());
    return Message{MessageType::ACK, request.request_id, {}};
}

Message Master::handle_read_file(const Message& request) {
    std::string path = bytes_to_string(request.payload);
    auto result = namespace_manager_.stat_file(path);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }

    // Faza 9 wire format: [u64 file_id][u64 size][u64 mtime][u32 block_count]
    // { [u64 block_id][u32 replica_count]
    //   {[u32 host_len][host][u16 port]} x replica_count } x block_count.
    // Client tries replicas in this order, falling back to the next on
    // failure (see docs/design.md §13).
    const FileMetadata& metadata = result.value();
    std::vector<std::byte> response_payload;
    put_u64(response_payload, metadata.id);
    put_u64(response_payload, metadata.size);
    put_u64(response_payload, metadata.modification_time);
    put_u32(response_payload, static_cast<uint32_t>(metadata.blocks.size()));
    for (BlockId block_id : metadata.blocks) {
        put_u64(response_payload, block_id);
        auto block_result = namespace_manager_.block_metadata(block_id);
        if (block_result.is_error()) {
            return make_error(request.request_id, block_result.error());
        }
        const std::vector<StorageNodeId>& replicas = block_result.value().replicas;
        put_u32(response_payload, static_cast<uint32_t>(replicas.size()));
        for (StorageNodeId node_id : replicas) {
            auto node = storage_registry_.get_node(node_id);
            if (!node.has_value()) {
                return make_error(request.request_id, ErrorCode::INTERNAL_ERROR);
            }
            put_string(response_payload, node->host);
            put_u16(response_payload, node->port);
        }
    }
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}

Message Master::handle_register_node(const Message& request) {
    // Wire format: [u64 node_id][u32 host_len][host][u16 port]
    // [u64 capacity_bytes]. Idempotent upsert — a node re-registering (e.g.
    // a manual restart during development) simply overwrites its prior
    // entry (see docs/design.md §13).
    size_t offset = 0;
    if (request.payload.size() < sizeof(uint64_t) + sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    StorageNodeId node_id = get_u64(request.payload.data() + offset);
    offset += sizeof(uint64_t);
    uint32_t host_length = get_u32(request.payload.data() + offset);
    offset += sizeof(uint32_t);

    if (request.payload.size() != offset + host_length + sizeof(uint16_t) + sizeof(uint64_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    std::string host(reinterpret_cast<const char*>(request.payload.data() + offset), host_length);
    offset += host_length;
    uint16_t port = get_u16(request.payload.data() + offset);
    offset += sizeof(uint16_t);
    uint64_t capacity_bytes = get_u64(request.payload.data() + offset);

    storage_registry_.register_node(
        StorageNodeInfo{node_id, std::move(host), port, /*alive=*/true, capacity_bytes, capacity_bytes});
    return Message{MessageType::ACK, request.request_id, {}};
}

Message Master::handle_append_file(const Message& request) {
    // Wire format: [u32 path_len][path][u64 append_size]. Response:
    // [u8 has_target], then if 1: [u64 block_id][u64 offset]
    // [u32 fill_amount][u32 replica_count]
    // {[u32 host_len][host][u16 port]} x replica_count — same replica list
    // shape as ALLOCATE_BLOCK's response (see docs/design.md §14).
    if (request.payload.size() < sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    uint32_t path_length = get_u32(request.payload.data());
    size_t offset = sizeof(uint32_t);
    if (request.payload.size() != offset + path_length + sizeof(uint64_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    std::string path(reinterpret_cast<const char*>(request.payload.data() + offset), path_length);
    offset += path_length;
    uint64_t append_size = get_u64(request.payload.data() + offset);

    auto result = namespace_manager_.resolve_append_target(path, append_size);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }
    const AppendTarget& target = result.value();

    std::vector<std::byte> response_payload;
    response_payload.push_back(static_cast<std::byte>(target.has_target ? 1 : 0));
    if (target.has_target) {
        put_u64(response_payload, target.block_id);
        put_u64(response_payload, target.offset);
        put_u32(response_payload, target.fill_amount);
        put_u32(response_payload, static_cast<uint32_t>(target.replicas.size()));
        for (StorageNodeId node_id : target.replicas) {
            auto node = storage_registry_.get_node(node_id);
            if (!node.has_value()) {
                return make_error(request.request_id, ErrorCode::INTERNAL_ERROR);
            }
            put_string(response_payload, node->host);
            put_u16(response_payload, node->port);
        }
    }
    return Message{MessageType::ACK, request.request_id, std::move(response_payload)};
}

Message Master::handle_append_commit(const Message& request) {
    // Wire format: [u32 path_len][path][u64 total_appended_bytes]
    // [u8 has_existing_target], then if 1: [u64 existing_block_id]
    // [u64 expected_prior_size][u32 filled_amount], then
    // [u32 new_block_count]{u64 block_id} x new_block_count
    // [u64 last_new_block_size] (meaningful only when new_block_count > 0 —
    // see docs/design.md §14).
    if (request.payload.size() < sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    uint32_t path_length = get_u32(request.payload.data());
    size_t offset = sizeof(uint32_t);
    if (request.payload.size() <
        offset + path_length + sizeof(uint64_t) + sizeof(uint8_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    std::string path(reinterpret_cast<const char*>(request.payload.data() + offset), path_length);
    offset += path_length;
    uint64_t total_appended_bytes = get_u64(request.payload.data() + offset);
    offset += sizeof(uint64_t);
    bool has_existing_target = static_cast<uint8_t>(request.payload[offset]) != 0;
    offset += sizeof(uint8_t);

    BlockId existing_block_id = 0;
    uint64_t expected_prior_size = 0;
    uint32_t filled_amount = 0;
    if (has_existing_target) {
        if (request.payload.size() < offset + sizeof(uint64_t) * 2 + sizeof(uint32_t)) {
            return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
        }
        existing_block_id = get_u64(request.payload.data() + offset);
        offset += sizeof(uint64_t);
        expected_prior_size = get_u64(request.payload.data() + offset);
        offset += sizeof(uint64_t);
        filled_amount = get_u32(request.payload.data() + offset);
        offset += sizeof(uint32_t);
    }

    if (request.payload.size() < offset + sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    uint32_t new_block_count = get_u32(request.payload.data() + offset);
    offset += sizeof(uint32_t);

    size_t expected_size =
        offset + static_cast<size_t>(new_block_count) * sizeof(uint64_t) + sizeof(uint64_t);
    if (request.payload.size() != expected_size) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }

    std::vector<BlockId> new_block_ids;
    new_block_ids.reserve(new_block_count);
    for (uint32_t i = 0; i < new_block_count; ++i) {
        new_block_ids.push_back(get_u64(request.payload.data() + offset));
        offset += sizeof(uint64_t);
    }
    uint64_t last_new_block_size = get_u64(request.payload.data() + offset);

    AppendCommitRequest commit_request{has_existing_target,   existing_block_id,
                                        expected_prior_size,   filled_amount,
                                        std::move(new_block_ids), last_new_block_size,
                                        total_appended_bytes};
    ErrorCode result = namespace_manager_.commit_append(path, commit_request);
    if (result != ErrorCode::OK) {
        return make_error(request.request_id, result);
    }
    return Message{MessageType::ACK, request.request_id, {}};
}

uint64_t Master::next_request_id() {
    return next_request_id_.fetch_add(1, std::memory_order_relaxed);
}

bool Master::send_delete_block(StorageNodeId node_id, BlockId block_id) {
    auto node = storage_registry_.get_node(node_id);
    if (!node.has_value()) {
        return false;
    }

    auto socket_result = Socket::create();
    if (socket_result.is_error()) {
        return false;
    }
    Socket socket = std::move(socket_result.value());
    // A node that accepts the connection but never replies would otherwise
    // block this worker thread (and, transitively, NetworkServer::stop())
    // forever — see docs/design.md §15.
    if (ErrorCode err = socket.set_timeout(DELETE_BLOCK_TIMEOUT_MS); err != ErrorCode::OK) {
        return false;
    }
    if (ErrorCode err = socket.connect(node->host, node->port); err != ErrorCode::OK) {
        return false;
    }
    TCPConnection node_connection(std::move(socket));

    std::vector<std::byte> payload;
    put_u64(payload, block_id);
    if (!node_connection.sendMessage(
            Message{MessageType::DELETE_BLOCK, next_request_id(), std::move(payload)})) {
        return false;
    }
    auto response = node_connection.receiveMessage();
    return response.has_value() && response->type == MessageType::ACK;
}

void Master::gc_dispatch(const std::vector<OrphanedBlock>& orphans) {
    // Best-effort: a node that's unreachable or doesn't ACK is skipped,
    // never surfaced as an error to the DELETE_FILE/WRITE_COMMIT caller —
    // see docs/design.md §15.
    for (const OrphanedBlock& orphan : orphans) {
        for (StorageNodeId node_id : orphan.replicas) {
            send_delete_block(node_id, orphan.id);
        }
    }
}
