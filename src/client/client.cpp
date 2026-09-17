#include "client/client.h"

#include <endian.h>

#include <algorithm>
#include <chrono>
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
    : config_(std::move(config)),
      master_connection_(std::move(master_connection)),
      cache_(config_.cache_max_bytes) {}

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

OperationId Client::next_operation_id() { return operation_id_rng_(); }

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
    auto start = std::chrono::steady_clock::now();
    ErrorCode result = create_file_impl(path);
    statistics_.record_operation(
        OperationType::CREATE, 0,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                start),
        result == ErrorCode::OK);
    return result;
}

ErrorCode Client::create_file_impl(const std::string& path) {
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

ErrorCode Client::delete_file(const std::string& path) {
    auto start = std::chrono::steady_clock::now();
    ErrorCode result = delete_file_impl(path);
    statistics_.record_operation(
        OperationType::DELETE, 0,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                start),
        result == ErrorCode::OK);
    return result;
}

ErrorCode Client::delete_file_impl(const std::string& path) {
    Message request{MessageType::DELETE_FILE, next_request_id(), string_to_bytes(path)};
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

Result<std::vector<std::string>> Client::list(const std::string& path) {
    auto start = std::chrono::steady_clock::now();
    auto result = list_impl(path);
    statistics_.record_operation(
        OperationType::LIST, 0,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                start),
        result.is_ok());
    return result;
}

Result<std::vector<std::string>> Client::list_impl(const std::string& path) {
    Message request{MessageType::LIST_FILES, next_request_id(), string_to_bytes(path)};
    auto result = call_master(request);
    if (result.is_error()) {
        return Result<std::vector<std::string>>::err(result.error());
    }
    const Message& response = result.value();
    if (response.type != MessageType::ACK) {
        return Result<std::vector<std::string>>::err(error_from_payload(response.payload));
    }

    // Wire format: [u32 count]{[u32 name_len][name]} x count (see
    // Master::handle_list_files, docs/design.md §15).
    const std::vector<std::byte>& payload = response.payload;
    if (payload.size() < sizeof(uint32_t)) {
        return Result<std::vector<std::string>>::err(ErrorCode::INTERNAL_ERROR);
    }
    uint32_t count = get_u32(payload.data());
    size_t offset = sizeof(uint32_t);
    std::vector<std::string> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (payload.size() < offset + sizeof(uint32_t)) {
            return Result<std::vector<std::string>>::err(ErrorCode::INTERNAL_ERROR);
        }
        uint32_t name_length = get_u32(payload.data() + offset);
        offset += sizeof(uint32_t);
        if (payload.size() < offset + name_length) {
            return Result<std::vector<std::string>>::err(ErrorCode::INTERNAL_ERROR);
        }
        entries.emplace_back(reinterpret_cast<const char*>(payload.data() + offset), name_length);
        offset += name_length;
    }
    if (payload.size() != offset) {
        return Result<std::vector<std::string>>::err(ErrorCode::INTERNAL_ERROR);
    }
    return Result<std::vector<std::string>>::ok(std::move(entries));
}

Result<Client::FileStat> Client::stat(const std::string& path) {
    auto start = std::chrono::steady_clock::now();
    auto result = stat_impl(path);
    statistics_.record_operation(
        OperationType::STAT, 0,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                start),
        result.is_ok());
    return result;
}

Result<Client::FileStat> Client::stat_impl(const std::string& path) {
    Message request{MessageType::STAT_FILE, next_request_id(), string_to_bytes(path)};
    auto result = call_master(request);
    if (result.is_error()) {
        return Result<FileStat>::err(result.error());
    }
    const Message& response = result.value();
    if (response.type != MessageType::ACK) {
        return Result<FileStat>::err(error_from_payload(response.payload));
    }

    // Wire format: [u64 id][u64 size][u64 mtime][u32 block_count]
    // {u64 block_id} x block_count (see Master::handle_stat_file,
    // docs/design.md §15).
    const std::vector<std::byte>& payload = response.payload;
    size_t offset = 3 * sizeof(uint64_t) + sizeof(uint32_t);
    if (payload.size() < offset) {
        return Result<FileStat>::err(ErrorCode::INTERNAL_ERROR);
    }
    FileStat stat_result;
    stat_result.id = get_u64(payload.data());
    stat_result.size = get_u64(payload.data() + sizeof(uint64_t));
    stat_result.modification_time = get_u64(payload.data() + 2 * sizeof(uint64_t));
    uint32_t block_count = get_u32(payload.data() + 3 * sizeof(uint64_t));

    if (payload.size() != offset + static_cast<size_t>(block_count) * sizeof(uint64_t)) {
        return Result<FileStat>::err(ErrorCode::INTERNAL_ERROR);
    }
    stat_result.blocks.reserve(block_count);
    for (uint32_t i = 0; i < block_count; ++i) {
        stat_result.blocks.push_back(get_u64(payload.data() + offset));
        offset += sizeof(uint64_t);
    }
    return Result<FileStat>::ok(std::move(stat_result));
}

Result<std::vector<Client::ReplicaLocation>> Client::parse_replica_list(
    const std::vector<std::byte>& payload, size_t& offset, uint32_t count) {
    std::vector<ReplicaLocation> replicas;
    replicas.reserve(count);
    for (uint32_t r = 0; r < count; ++r) {
        if (payload.size() < offset + sizeof(uint32_t)) {
            return Result<std::vector<ReplicaLocation>>::err(ErrorCode::INTERNAL_ERROR);
        }
        uint32_t host_length = get_u32(payload.data() + offset);
        offset += sizeof(uint32_t);
        if (payload.size() < offset + host_length + sizeof(uint16_t)) {
            return Result<std::vector<ReplicaLocation>>::err(ErrorCode::INTERNAL_ERROR);
        }
        std::string host(reinterpret_cast<const char*>(payload.data() + offset), host_length);
        offset += host_length;
        uint16_t port = get_u16(payload.data() + offset);
        offset += sizeof(uint16_t);
        replicas.push_back(ReplicaLocation{std::move(host), port});
    }
    return Result<std::vector<ReplicaLocation>>::ok(std::move(replicas));
}

ErrorCode Client::write_chunk_to_replicas(BlockId block_id, uint64_t offset, uint64_t block_version,
                                           const uint8_t* data, size_t len,
                                           const std::vector<ReplicaLocation>& replicas) {
    // Data plane — never routed through Master, one fresh connection per
    // replica (no pooling, see docs/design.md §11). Commit requires ALL
    // replicas to ACK (strict 3/3, see docs/design.md §13), so abort
    // immediately on the first failure — no point trying the remaining
    // replicas once commit is already impossible.
    //
    // One operation_id per logical chunk-write, reused across all replica
    // sends — lets a Storage Node retry-detect this exact chunk-write
    // regardless of which replica a retry lands on (see docs/design.md
    // §16).
    OperationId operation_id = next_operation_id();
    for (const ReplicaLocation& location : replicas) {
        auto socket_result = Socket::create();
        if (socket_result.is_error()) {
            return socket_result.error();
        }
        Socket socket = std::move(socket_result.value());
        if (ErrorCode err = socket.connect(location.host, location.port); err != ErrorCode::OK) {
            return err;
        }
        TCPConnection node_connection(std::move(socket));

        std::vector<std::byte> write_payload;
        put_u64(write_payload, operation_id);
        put_u64(write_payload, block_id);
        put_u64(write_payload, offset);
        put_u64(write_payload, block_version);
        put_u32(write_payload, static_cast<uint32_t>(len));
        const auto* data_bytes = reinterpret_cast<const std::byte*>(data);
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
            // (see docs/design.md §12); already-written replicas for this
            // attempt become orphans (GC deferred).
            return error_from_payload(write_response->payload);
        }
    }
    return ErrorCode::OK;
}

Result<std::vector<BlockId>> Client::write_new_blocks(const std::string& path, const uint8_t* data,
                                                       size_t len) {
    size_t chunk_count = len == 0 ? 0 : (len + config_.block_size - 1) / config_.block_size;
    std::vector<BlockId> block_ids;
    block_ids.reserve(chunk_count);

    for (size_t i = 0; i < chunk_count; ++i) {
        size_t chunk_offset = i * config_.block_size;
        size_t chunk_len = std::min<size_t>(config_.block_size, len - chunk_offset);

        // Step 1: ask Master for a BlockId + replica locations.
        Message alloc_request{MessageType::ALLOCATE_BLOCK, next_request_id(), string_to_bytes(path)};
        auto alloc_result = call_master(alloc_request);
        if (alloc_result.is_error()) {
            return Result<std::vector<BlockId>>::err(alloc_result.error());
        }
        const Message& alloc_response = alloc_result.value();
        if (alloc_response.type != MessageType::ACK) {
            return Result<std::vector<BlockId>>::err(error_from_payload(alloc_response.payload));
        }
        if (alloc_response.payload.size() < sizeof(uint64_t) + sizeof(uint32_t)) {
            return Result<std::vector<BlockId>>::err(ErrorCode::INTERNAL_ERROR);
        }
        BlockId block_id = get_u64(alloc_response.payload.data());
        uint32_t replica_count = get_u32(alloc_response.payload.data() + sizeof(uint64_t));
        size_t resp_offset = sizeof(uint64_t) + sizeof(uint32_t);

        auto replicas_result = parse_replica_list(alloc_response.payload, resp_offset, replica_count);
        if (replicas_result.is_error()) {
            return Result<std::vector<BlockId>>::err(replicas_result.error());
        }
        // Faza 12.3: trailing [u64 version] — always 1 for a freshly
        // allocated block (see docs/design.md §16, Master::handle_allocate_block).
        if (alloc_response.payload.size() != resp_offset + sizeof(uint64_t)) {
            return Result<std::vector<BlockId>>::err(ErrorCode::INTERNAL_ERROR);
        }
        uint64_t block_version = get_u64(alloc_response.payload.data() + resp_offset);
        resp_offset += sizeof(uint64_t);

        // Step 2: write this chunk (as a brand-new block, offset 0) to
        // every replica.
        ErrorCode write_err =
            write_chunk_to_replicas(block_id, /*offset=*/0, block_version, data + chunk_offset,
                                     chunk_len, replicas_result.value());
        if (write_err != ErrorCode::OK) {
            return Result<std::vector<BlockId>>::err(write_err);
        }

        block_ids.push_back(block_id);
    }
    return Result<std::vector<BlockId>>::ok(std::move(block_ids));
}

ErrorCode Client::write(const std::string& path, const std::vector<uint8_t>& data) {
    auto start = std::chrono::steady_clock::now();
    ErrorCode result = write_impl(path, data);
    statistics_.record_write(
        data.size(),
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                start),
        result == ErrorCode::OK);
    return result;
}

ErrorCode Client::write_impl(const std::string& path, const std::vector<uint8_t>& data) {
    // block_size must be positive and never exceed the real BLOCK_SIZE — a
    // larger chunk would exceed MAX_DATA_MESSAGE_SIZE at the network layer
    // and fail with a confusing INTERNAL_ERROR deep inside TCPConnection
    // instead of a clean INVALID_REQUEST here.
    if (config_.block_size == 0 || config_.block_size > BLOCK_SIZE) {
        return ErrorCode::INVALID_REQUEST;
    }

    auto blocks_result = write_new_blocks(path, data.data(), data.size());
    if (blocks_result.is_error()) {
        return blocks_result.error();
    }
    const std::vector<BlockId>& block_ids = blocks_result.value();

    uint64_t total = data.size();
    uint64_t last_block_size = 0;
    if (!block_ids.empty()) {
        last_block_size = total - static_cast<uint64_t>(block_ids.size() - 1) * config_.block_size;
    }

    // Step 3: tell Master the whole ordered block list is committed, so it
    // becomes visible via STAT_FILE/READ_FILE in one atomic step.
    // last_block_size lets Master track how full the last block is, which
    // append() needs to compute its offset (see docs/design.md §14).
    std::vector<std::byte> commit_payload;
    put_string(commit_payload, path);
    put_u64(commit_payload, total);
    put_u32(commit_payload, static_cast<uint32_t>(block_ids.size()));
    for (BlockId block_id : block_ids) {
        put_u64(commit_payload, block_id);
    }
    put_u64(commit_payload, last_block_size);

    Message commit_request{MessageType::WRITE_COMMIT, next_request_id(), std::move(commit_payload)};
    auto commit_result = call_master(commit_request);
    if (commit_result.is_error()) {
        return commit_result.error();
    }
    if (commit_result.value().type != MessageType::ACK) {
        return error_from_payload(commit_result.value().payload);
    }

    // Faza 13: invalidate any cache entry for these block ids now that
    // they're committed under new content (see docs/design.md §17). write()
    // always allocates brand-new BlockIds (Faza 11 overwrite semantics), so
    // this is a no-op today — kept for symmetry with append()'s real hazard
    // and to stay correct if that ever changes.
    for (BlockId block_id : block_ids) {
        cache_.invalidate(block_id);
    }
    return ErrorCode::OK;
}

ErrorCode Client::append(const std::string& path, const std::vector<uint8_t>& data) {
    auto start = std::chrono::steady_clock::now();
    ErrorCode result = append_impl(path, data);
    statistics_.record_append(
        data.size(),
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                start),
        result == ErrorCode::OK);
    return result;
}

ErrorCode Client::append_impl(const std::string& path, const std::vector<uint8_t>& data) {
    if (config_.block_size == 0 || config_.block_size > BLOCK_SIZE) {
        return ErrorCode::INVALID_REQUEST;
    }
    if (data.empty()) {
        return ErrorCode::OK;
    }

    // Step 1: ask Master how much of this append fits into the file's
    // current last block (see docs/design.md §14).
    std::vector<std::byte> target_request_payload;
    put_string(target_request_payload, path);
    put_u64(target_request_payload, static_cast<uint64_t>(data.size()));
    Message target_request{MessageType::APPEND_FILE, next_request_id(),
                            std::move(target_request_payload)};
    auto target_result = call_master(target_request);
    if (target_result.is_error()) {
        return target_result.error();
    }
    const Message& target_response = target_result.value();
    if (target_response.type != MessageType::ACK) {
        return error_from_payload(target_response.payload);
    }
    if (target_response.payload.empty()) {
        return ErrorCode::INTERNAL_ERROR;
    }

    bool has_target = static_cast<uint8_t>(target_response.payload[0]) != 0;
    size_t resp_offset = 1;
    BlockId existing_block_id = 0;
    uint64_t expected_prior_size = 0;
    uint32_t fill_amount = 0;
    uint64_t target_version = 0;
    if (has_target) {
        if (target_response.payload.size() < resp_offset + 2 * sizeof(uint64_t) + 2 * sizeof(uint32_t)) {
            return ErrorCode::INTERNAL_ERROR;
        }
        existing_block_id = get_u64(target_response.payload.data() + resp_offset);
        resp_offset += sizeof(uint64_t);
        expected_prior_size = get_u64(target_response.payload.data() + resp_offset);
        resp_offset += sizeof(uint64_t);
        fill_amount = get_u32(target_response.payload.data() + resp_offset);
        resp_offset += sizeof(uint32_t);
        uint32_t replica_count = get_u32(target_response.payload.data() + resp_offset);
        resp_offset += sizeof(uint32_t);

        auto replicas_result = parse_replica_list(target_response.payload, resp_offset, replica_count);
        if (replicas_result.is_error()) {
            return replicas_result.error();
        }
        // Faza 12.3: trailing [u64 target_version] (see docs/design.md §16,
        // Master::handle_append_file).
        if (target_response.payload.size() != resp_offset + sizeof(uint64_t)) {
            return ErrorCode::INTERNAL_ERROR;
        }
        target_version = get_u64(target_response.payload.data() + resp_offset);
        resp_offset += sizeof(uint64_t);

        if (fill_amount > 0) {
            ErrorCode write_err =
                write_chunk_to_replicas(existing_block_id, expected_prior_size, target_version,
                                         data.data(), fill_amount, replicas_result.value());
            if (write_err != ErrorCode::OK) {
                return write_err;
            }
            // Faza 13 fix: invalidate right after the fill
            // physically lands, not after APPEND_COMMIT succeeds. The
            // Storage Node's on-disk bytes for this block are already
            // stale-for-the-cache at this point regardless of whether the
            // commit that follows succeeds or fails — waiting for the
            // commit left a window where a commit failure would return an
            // error while leaving this entry's now-wrong cached bytes in
            // place (see docs/design.md §17).
            cache_.invalidate(existing_block_id);
        }
    } else if (target_response.payload.size() != resp_offset) {
        return ErrorCode::INTERNAL_ERROR;
    }

    // Step 2: anything the existing block couldn't absorb (or the whole
    // append, on an empty file / already-full last block, see docs/design.md
    // §14 §6.11) goes through the same allocate-and-write chunk loop write()
    // uses.
    std::vector<BlockId> new_block_ids;
    uint64_t remainder_len = data.size() - fill_amount;
    if (remainder_len > 0) {
        auto rest_result = write_new_blocks(path, data.data() + fill_amount, remainder_len);
        if (rest_result.is_error()) {
            return rest_result.error();
        }
        new_block_ids = std::move(rest_result.value());
    }

    // The last of the new blocks is the only one that can ever be an
    // append target again, so it's the only one whose real size Master
    // needs (see docs/design.md §14) — same reasoning as write()'s
    // last_block_size.
    uint64_t last_new_block_size = 0;
    if (!new_block_ids.empty()) {
        last_new_block_size =
            remainder_len - static_cast<uint64_t>(new_block_ids.size() - 1) * config_.block_size;
    }

    // Step 3: one commit covering both the fill (if any) and the new blocks.
    std::vector<std::byte> commit_payload;
    put_string(commit_payload, path);
    put_u64(commit_payload, static_cast<uint64_t>(data.size()));
    commit_payload.push_back(static_cast<std::byte>(has_target ? 1 : 0));
    if (has_target) {
        put_u64(commit_payload, existing_block_id);
        put_u64(commit_payload, expected_prior_size);
        put_u32(commit_payload, fill_amount);
        put_u64(commit_payload, target_version);
    }
    put_u32(commit_payload, static_cast<uint32_t>(new_block_ids.size()));
    for (BlockId block_id : new_block_ids) {
        put_u64(commit_payload, block_id);
    }
    put_u64(commit_payload, last_new_block_size);

    Message commit_request{MessageType::APPEND_COMMIT, next_request_id(), std::move(commit_payload)};
    auto commit_result = call_master(commit_request);
    if (commit_result.is_error()) {
        return commit_result.error();
    }
    if (commit_result.value().type != MessageType::ACK) {
        return error_from_payload(commit_result.value().payload);
    }

    // Faza 13: invalidate any newly allocated blocks (no-op today — see
    // write()'s comment above — kept for symmetry). The existing target
    // block, if any, was already invalidated right after its fill write
    // landed, above.
    for (BlockId block_id : new_block_ids) {
        cache_.invalidate(block_id);
    }
    return ErrorCode::OK;
}

Result<std::vector<uint8_t>> Client::read(const std::string& path) {
    auto start = std::chrono::steady_clock::now();
    auto result = read_impl(path);
    uint64_t bytes = result.is_ok() ? result.value().size() : 0;
    statistics_.record_read(
        bytes,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                start),
        result.is_ok());
    return result;
}

Result<std::vector<uint8_t>> Client::read_impl(const std::string& path) {
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

    // Parse every block's (block_id, replica list) entry, in order — each
    // block now carries a list of candidate replicas rather than exactly
    // one location (see docs/design.md §13).
    struct Location {
        std::string host;
        uint16_t port;
    };
    struct BlockLocations {
        BlockId block_id;
        std::vector<Location> replicas;
    };
    std::vector<BlockLocations> blocks;
    blocks.reserve(block_count);
    size_t offset = 3 * sizeof(uint64_t) + sizeof(uint32_t);
    for (uint32_t i = 0; i < block_count; ++i) {
        if (meta_response.payload.size() < offset + sizeof(uint64_t) + sizeof(uint32_t)) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
        }
        BlockId block_id = get_u64(meta_response.payload.data() + offset);
        offset += sizeof(uint64_t);
        uint32_t replica_count = get_u32(meta_response.payload.data() + offset);
        offset += sizeof(uint32_t);

        std::vector<Location> replicas;
        replicas.reserve(replica_count);
        for (uint32_t r = 0; r < replica_count; ++r) {
            if (meta_response.payload.size() < offset + sizeof(uint32_t)) {
                return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
            }
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
            replicas.push_back(Location{std::move(host), port});
        }
        blocks.push_back(BlockLocations{block_id, std::move(replicas)});
    }
    if (meta_response.payload.size() != offset) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::INTERNAL_ERROR);
    }

    // Step 2: read every block directly from a Storage Node, in order, and
    // concatenate. Try replicas in the order Master returned them, falling
    // back to the next on any failure (connect failure, non-ACK response,
    // or a malformed response) — fail the whole read only if every replica
    // for a block fails (see docs/design.md §13). A fresh connection per
    // attempt, consistent with the existing no-pooling decision (see
    // docs/design.md §11).
    std::vector<uint8_t> result;
    result.reserve(total_size);
    for (const BlockLocations& block : blocks) {
        // Faza 13: a cache hit is trusted unconditionally and never touches
        // the network — this Client is the sole writer of blocks it
        // caches, and already invalidates them on its own successful
        // write()/append() (see docs/design.md §17).
        if (auto cached = cache_.get(block.block_id); cached.has_value()) {
            result.insert(result.end(), cached->data.begin(), cached->data.end());
            continue;
        }

        bool block_read = false;
        for (const Location& location : block.replicas) {
            auto socket_result = Socket::create();
            if (socket_result.is_error()) {
                continue;
            }
            Socket socket = std::move(socket_result.value());
            if (ErrorCode err = socket.connect(location.host, location.port); err != ErrorCode::OK) {
                continue;
            }
            TCPConnection node_connection(std::move(socket));

            // known_version=0: this is a cache miss, so there is nothing to
            // validate against — always fetch the full block (see
            // docs/design.md §17).
            std::vector<std::byte> read_payload;
            put_u64(read_payload, block.block_id);
            put_u64(read_payload, /*known_version=*/0);
            if (!node_connection.sendMessage(
                    Message{MessageType::READ_BLOCK, next_request_id(), std::move(read_payload)})) {
                continue;
            }
            auto read_response = node_connection.receiveMessage();
            if (!read_response.has_value() || read_response->type != MessageType::READ_OK) {
                continue;
            }
            if (read_response->payload.size() < sizeof(uint64_t) + sizeof(uint32_t)) {
                continue;
            }
            uint64_t version = get_u64(read_response->payload.data());
            uint32_t data_size = get_u32(read_response->payload.data() + sizeof(uint64_t));
            size_t data_offset = sizeof(uint64_t) + sizeof(uint32_t);
            if (read_response->payload.size() != data_offset + data_size) {
                continue;
            }

            const auto* block_bytes = read_response->payload.data() + data_offset;
            std::vector<uint8_t> block_data(reinterpret_cast<const uint8_t*>(block_bytes),
                                             reinterpret_cast<const uint8_t*>(block_bytes) + data_size);
            cache_.put(block.block_id, version, block_data);
            result.insert(result.end(), block_data.begin(), block_data.end());
            block_read = true;
            break;
        }
        if (!block_read) {
            return Result<std::vector<uint8_t>>::err(ErrorCode::BLOCK_NOT_FOUND);
        }
    }
    return Result<std::vector<uint8_t>>::ok(std::move(result));
}

StatisticsSnapshot Client::statistics_snapshot() const { return statistics_.snapshot(); }

CacheStats Client::cache_stats() const { return cache_.stats(); }
