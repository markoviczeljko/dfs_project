#include "master/recovery_manager.h"

#include <endian.h>

#include <cstring>
#include <utility>

#include "common/config.h"
#include "network/connection.h"
#include "network/socket.h"

namespace {

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

}  // namespace

RecoveryManager::RecoveryManager(NamespaceManager& namespace_manager, StorageRegistry& registry,
                                  ReplicationManager& replication_manager,
                                  std::function<void()> on_metadata_changed)
    : namespace_manager_(namespace_manager),
      registry_(registry),
      replication_manager_(replication_manager),
      on_metadata_changed_(std::move(on_metadata_changed)) {}

RecoveryManager::~RecoveryManager() { stop(); }

void RecoveryManager::start() {
    if (running_.exchange(true)) return;  // already running
    workers_.reserve(MAX_CONCURRENT_RECOVERY_OPERATIONS);
    for (int i = 0; i < MAX_CONCURRENT_RECOVERY_OPERATIONS; ++i) {
        workers_.emplace_back(&RecoveryManager::worker_loop, this);
    }
}

void RecoveryManager::stop() {
    if (!running_.exchange(false)) return;  // already stopped
    queue_cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
}

void RecoveryManager::handle_node_failure(StorageNodeId node_id) {
    std::vector<BlockId> affected = namespace_manager_.blocks_with_replica(node_id);
    for (BlockId block_id : affected) {
        namespace_manager_.mark_under_replicated(block_id);
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(RecoveryJob{block_id, node_id});
        }
        queue_cv_.notify_one();
    }
}

void RecoveryManager::worker_loop() {
    while (true) {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
        if (!running_.load()) return;
        RecoveryJob job = queue_.front();
        queue_.pop_front();
        lock.unlock();

        recover_block(job);
    }
}

void RecoveryManager::recover_block(RecoveryJob job) {
    for (int attempt = 1; attempt <= MAX_RECOVERY_ATTEMPTS_PER_BLOCK; ++attempt) {
        auto metadata = namespace_manager_.block_metadata(job.block_id);
        if (metadata.is_error()) {
            return;  // block gone (DELETE/overwrite raced ahead of us) — nothing to recover
        }

        StorageNodeId source = 0;
        bool found_source = false;
        for (StorageNodeId candidate : metadata.value().replicas) {
            if (candidate == job.failed_node_id) continue;
            auto info = registry_.get_node(candidate);
            if (info.has_value() && info->state == NodeState::HEALTHY) {
                source = candidate;
                found_source = true;
                break;
            }
        }
        if (!found_source) continue;  // no healthy source available right now — retry

        auto destination = replication_manager_.choose_recovery_destination(metadata.value().replicas);
        if (!destination.has_value()) continue;  // no destination available right now — retry

        OperationId operation_id = next_operation_id();
        if (send_copy_block(source, job.block_id, metadata.value().version, *destination,
                             operation_id)) {
            namespace_manager_.update_block_replicas_after_recovery(
                job.block_id, job.failed_node_id, *destination, BlockState::COMMITTED);
            on_metadata_changed_();
            return;
        }
        // Failed (source/destination died mid-transfer, checksum mismatch,
        // etc.) — loop retries with a fresh source/destination pick (see
        // docs/design.md §8.39).
    }
    // Gave up — block stays UNDER_REPLICATED; a later heartbeat/inventory
    // event may re-trigger recovery (see docs/design.md §16).
}

bool RecoveryManager::send_copy_block(StorageNodeId source, BlockId block_id,
                                       uint64_t expected_version, StorageNodeId destination,
                                       OperationId operation_id) {
    auto source_node = registry_.get_node(source);
    auto dest_node = registry_.get_node(destination);
    if (!source_node.has_value() || !dest_node.has_value()) {
        return false;
    }

    auto socket_result = Socket::create();
    if (socket_result.is_error()) {
        return false;
    }
    Socket socket = std::move(socket_result.value());
    // Bounds THIS leg (Master->source); the source->destination leg is
    // independently bounded inside StorageNode::handle_copy_block() — see
    // docs/design.md §16 and the Faza 11 CRITICAL-bug lesson this mirrors.
    if (socket.set_timeout(COPY_BLOCK_TIMEOUT_MS) != ErrorCode::OK) {
        return false;
    }
    if (socket.connect(source_node->host, source_node->port) != ErrorCode::OK) {
        return false;
    }
    TCPConnection source_connection(std::move(socket));

    std::vector<std::byte> payload;
    put_u64(payload, operation_id);
    put_u64(payload, block_id);
    put_u64(payload, expected_version);
    put_u32(payload, static_cast<uint32_t>(dest_node->host.size()));
    const auto* host_bytes = reinterpret_cast<const std::byte*>(dest_node->host.data());
    payload.insert(payload.end(), host_bytes, host_bytes + dest_node->host.size());
    put_u16(payload, dest_node->port);
    put_u64(payload, destination);

    if (!source_connection.sendMessage(
            Message{MessageType::COPY_BLOCK, operation_id, std::move(payload)})) {
        return false;
    }
    auto response = source_connection.receiveMessage();
    return response.has_value() && response->type == MessageType::ACK;
}

OperationId RecoveryManager::next_operation_id() {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    return operation_id_rng_();
}
