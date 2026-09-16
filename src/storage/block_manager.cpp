#include "storage/block_manager.h"

#include <utility>

BlockManager::BlockManager(std::unique_ptr<BlockStorage> storage) : storage_(std::move(storage)) {}

Result<std::unique_ptr<BlockManager>> BlockManager::create(std::filesystem::path directory,
                                                            uint64_t capacity_bytes) {
    auto storage_result = BlockStorage::create(std::move(directory), capacity_bytes);
    if (storage_result.is_error()) {
        return Result<std::unique_ptr<BlockManager>>::err(storage_result.error());
    }

    auto manager = std::unique_ptr<BlockManager>(new BlockManager(std::move(storage_result.value())));
    return Result<std::unique_ptr<BlockManager>>::ok(std::move(manager));
}

Result<BlockId> BlockManager::allocate_block() {
    std::lock_guard<std::mutex> lock(mutex_);
    BlockId id = next_id_++;
    allocated_ids_.insert(id);
    return Result<BlockId>::ok(id);
}

ErrorCode BlockManager::release_block(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    allocated_ids_.erase(id);
    storage_->delete_block(id);
    return ErrorCode::OK;
}

ErrorCode BlockManager::write_block(BlockId id, const std::vector<uint8_t>& data,
                                     uint64_t block_version) {
    return storage_->write_block(id, data, block_version);
}

Result<std::vector<uint8_t>> BlockManager::read_block(BlockId id) {
    return storage_->read_block(id);
}

ErrorCode BlockManager::write_block_at(BlockId id, uint64_t offset, const std::vector<uint8_t>& data,
                                        uint64_t block_version) {
    return storage_->write_block_at(id, offset, data, block_version);
}

Result<BlockStorage::BlockInfo> BlockManager::get_block_info(BlockId id) const {
    return storage_->get_block_info(id);
}

std::vector<std::pair<BlockId, BlockStorage::BlockInfo>> BlockManager::enumerate_blocks() const {
    return storage_->enumerate_blocks();
}
