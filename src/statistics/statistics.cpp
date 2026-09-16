#include "statistics/statistics.h"

#include <sstream>

namespace {

size_t operation_index(OperationType type) { return static_cast<size_t>(type); }

std::string_view operation_name(OperationType type) {
    switch (type) {
        case OperationType::READ:
            return "READ";
        case OperationType::WRITE:
            return "WRITE";
        case OperationType::APPEND:
            return "APPEND";
        case OperationType::CREATE:
            return "CREATE";
        case OperationType::DELETE:
            return "DELETE";
        case OperationType::LIST:
            return "LIST";
        case OperationType::STAT:
            return "STAT";
    }
    return "UNKNOWN";
}

std::string_view error_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::OK:
            return "OK";
        case ErrorCode::INVALID_REQUEST:
            return "INVALID_REQUEST";
        case ErrorCode::INVALID_MESSAGE:
            return "INVALID_MESSAGE";
        case ErrorCode::FILE_NOT_FOUND:
            return "FILE_NOT_FOUND";
        case ErrorCode::FILE_ALREADY_EXISTS:
            return "FILE_ALREADY_EXISTS";
        case ErrorCode::BLOCK_NOT_FOUND:
            return "BLOCK_NOT_FOUND";
        case ErrorCode::NODE_NOT_FOUND:
            return "NODE_NOT_FOUND";
        case ErrorCode::BLOCK_CORRUPTED:
            return "BLOCK_CORRUPTED";
        case ErrorCode::STORAGE_FULL:
            return "STORAGE_FULL";
        case ErrorCode::TIMEOUT:
            return "TIMEOUT";
        case ErrorCode::INTERNAL_ERROR:
            return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

}  // namespace

void StatisticsManager::record_operation(OperationType type, uint64_t bytes,
                                          std::chrono::microseconds latency, bool success) {
    OperationCounters& counters = operations_[operation_index(type)];
    counters.total.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        counters.successful.fetch_add(1, std::memory_order_relaxed);
    } else {
        counters.failed.fetch_add(1, std::memory_order_relaxed);
    }
    counters.bytes.fetch_add(bytes, std::memory_order_relaxed);
    counters.latency.record(latency);
}

void StatisticsManager::record_read(uint64_t bytes, std::chrono::microseconds latency,
                                     bool success) {
    record_operation(OperationType::READ, bytes, latency, success);
}

void StatisticsManager::record_write(uint64_t bytes, std::chrono::microseconds latency,
                                      bool success) {
    record_operation(OperationType::WRITE, bytes, latency, success);
}

void StatisticsManager::record_append(uint64_t bytes, std::chrono::microseconds latency,
                                       bool success) {
    record_operation(OperationType::APPEND, bytes, latency, success);
}

void StatisticsManager::record_error(ErrorCode code) {
    size_t index = static_cast<size_t>(code);
    if (index < errors_by_code_.size()) {
        errors_by_code_[index].fetch_add(1, std::memory_order_relaxed);
    }
}

void StatisticsManager::record_replication(uint64_t bytes) {
    replication_operations_.fetch_add(1, std::memory_order_relaxed);
    bytes_replicated_.fetch_add(bytes, std::memory_order_relaxed);
}

void StatisticsManager::record_recovery(uint64_t bytes, bool success) {
    recovery_operations_.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        successful_recoveries_.fetch_add(1, std::memory_order_relaxed);
        bytes_recovered_.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void StatisticsManager::record_node_dead() {
    detected_failures_.fetch_add(1, std::memory_order_relaxed);
}

void StatisticsManager::record_heartbeat_sent(bool success) {
    heartbeats_sent_.fetch_add(1, std::memory_order_relaxed);
    if (!success) {
        heartbeat_send_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

void StatisticsManager::record_heartbeat_received() {
    heartbeats_received_.fetch_add(1, std::memory_order_relaxed);
}

void StatisticsManager::record_storage_capacity(uint64_t capacity_bytes, uint64_t used_bytes,
                                                 uint64_t block_count) {
    std::lock_guard<std::mutex> lock(capacity_mutex_);
    storage_capacity_bytes_ = capacity_bytes;
    storage_used_bytes_ = used_bytes;
    storage_block_count_ = block_count;
}

StatisticsSnapshot StatisticsManager::snapshot() const {
    StatisticsSnapshot snap;
    for (size_t i = 0; i < kNumOperationTypes; ++i) {
        const OperationCounters& counters = operations_[i];
        OperationStats& out = snap.operations[i];
        out.total = counters.total.load(std::memory_order_relaxed);
        out.successful = counters.successful.load(std::memory_order_relaxed);
        out.failed = counters.failed.load(std::memory_order_relaxed);
        out.bytes = counters.bytes.load(std::memory_order_relaxed);
        out.p50_us = counters.latency.percentile(0.50);
        out.p90_us = counters.latency.percentile(0.90);
        out.p95_us = counters.latency.percentile(0.95);
        out.p99_us = counters.latency.percentile(0.99);
    }
    for (size_t i = 0; i < errors_by_code_.size(); ++i) {
        snap.errors_by_code[i] = errors_by_code_[i].load(std::memory_order_relaxed);
    }

    snap.replication_operations = replication_operations_.load(std::memory_order_relaxed);
    snap.bytes_replicated = bytes_replicated_.load(std::memory_order_relaxed);

    snap.recovery_operations = recovery_operations_.load(std::memory_order_relaxed);
    snap.successful_recoveries = successful_recoveries_.load(std::memory_order_relaxed);
    snap.bytes_recovered = bytes_recovered_.load(std::memory_order_relaxed);

    snap.detected_failures = detected_failures_.load(std::memory_order_relaxed);

    snap.heartbeats_sent = heartbeats_sent_.load(std::memory_order_relaxed);
    snap.heartbeat_send_failures = heartbeat_send_failures_.load(std::memory_order_relaxed);
    snap.heartbeats_received = heartbeats_received_.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(capacity_mutex_);
        snap.storage_capacity_bytes = storage_capacity_bytes_;
        snap.storage_used_bytes = storage_used_bytes_;
        snap.storage_block_count = storage_block_count_;
    }

    return snap;
}

std::string StatisticsManager::format_snapshot() const {
    StatisticsSnapshot snap = snapshot();
    std::ostringstream out;
    out << "DFS Statistics\n";
    out << "==============\n";
    out << "Operations:\n";
    for (size_t i = 0; i < kNumOperationTypes; ++i) {
        const OperationStats& op = snap.operations[i];
        out << "  " << operation_name(static_cast<OperationType>(i)) << ": total=" << op.total
            << " successful=" << op.successful << " failed=" << op.failed
            << " bytes=" << op.bytes << " p50_us=" << op.p50_us << " p90_us=" << op.p90_us
            << " p95_us=" << op.p95_us << " p99_us=" << op.p99_us << "\n";
    }
    out << "Errors:\n";
    for (size_t i = 0; i < snap.errors_by_code.size(); ++i) {
        if (snap.errors_by_code[i] == 0) continue;
        out << "  " << error_name(static_cast<ErrorCode>(i)) << ": " << snap.errors_by_code[i]
            << "\n";
    }
    out << "Replication: operations=" << snap.replication_operations
        << " bytes=" << snap.bytes_replicated << "\n";
    out << "Recovery: operations=" << snap.recovery_operations
        << " successful=" << snap.successful_recoveries << " bytes=" << snap.bytes_recovered
        << "\n";
    out << "Failures: detected=" << snap.detected_failures << "\n";
    out << "Heartbeats: sent=" << snap.heartbeats_sent
        << " send_failures=" << snap.heartbeat_send_failures
        << " received=" << snap.heartbeats_received << "\n";
    out << "Storage: capacity_bytes=" << snap.storage_capacity_bytes
        << " used_bytes=" << snap.storage_used_bytes
        << " block_count=" << snap.storage_block_count << "\n";
    return out.str();
}
