#include "storage/operation_log.h"

OperationLog::OperationLog(size_t capacity) : capacity_(capacity) {}

std::optional<OperationRecord> OperationLog::begin_or_get(OperationId id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = records_.find(id);
    if (it != records_.end()) {
        return it->second;
    }

    records_[id] = OperationRecord{id, OperationState::IN_PROGRESS, ErrorCode::OK, {}};
    insertion_order_.push_back(id);
    if (insertion_order_.size() > capacity_) {
        OperationId oldest = insertion_order_.front();
        insertion_order_.pop_front();
        records_.erase(oldest);
    }
    return std::nullopt;
}

void OperationLog::complete(OperationId id, ErrorCode result, std::vector<std::byte> response_payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(id);
    if (it == records_.end()) {
        // Evicted while in flight (capacity pressure) — nothing to update,
        // a retry of this id will just be treated as brand-new.
        return;
    }
    it->second.state = (result == ErrorCode::OK) ? OperationState::COMPLETED : OperationState::FAILED;
    it->second.result = result;
    it->second.response_payload = std::move(response_payload);
}
