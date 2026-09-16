#include "storage/block_storage.h"

#include <endian.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <system_error>

#include "common/config.h"
#include "storage/block_header.h"

namespace {

void put_u16(std::byte* dest, uint16_t value) {
    uint16_t be = htobe16(value);
    std::memcpy(dest, &be, sizeof(be));
}

void put_u32(std::byte* dest, uint32_t value) {
    uint32_t be = htobe32(value);
    std::memcpy(dest, &be, sizeof(be));
}

void put_u64(std::byte* dest, uint64_t value) {
    uint64_t be = htobe64(value);
    std::memcpy(dest, &be, sizeof(be));
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

bool full_pwrite(int fd, const void* data, size_t size, off_t offset) {
    const char* cursor = static_cast<const char*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t n = ::pwrite(fd, cursor, remaining, offset);
        if (n <= 0) return false;
        remaining -= static_cast<size_t>(n);
        cursor += n;
        offset += n;
    }
    return true;
}

bool full_pread(int fd, void* buffer, size_t size, off_t offset) {
    char* cursor = static_cast<char*>(buffer);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t n = ::pread(fd, cursor, remaining, offset);
        if (n <= 0) return false;
        remaining -= static_cast<size_t>(n);
        cursor += n;
        offset += n;
    }
    return true;
}

}  // namespace

uint32_t compute_block_checksum(const std::vector<uint8_t>& data) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t byte : data) {
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::array<std::byte, BLOCK_HEADER_SIZE> serialize_block_header(const BlockHeader& header) {
    std::array<std::byte, BLOCK_HEADER_SIZE> bytes{};
    put_u32(bytes.data(), header.magic);
    put_u16(bytes.data() + 4, header.version);
    put_u64(bytes.data() + 6, header.block_id);
    put_u32(bytes.data() + 14, header.data_size);
    put_u32(bytes.data() + 18, header.checksum);
    put_u64(bytes.data() + 22, header.block_version);
    return bytes;
}

Result<BlockHeader> deserialize_block_header(std::span<const std::byte, BLOCK_HEADER_SIZE> bytes) {
    BlockHeader header{};
    header.magic = get_u32(bytes.data());
    header.version = get_u16(bytes.data() + 4);
    header.block_id = get_u64(bytes.data() + 6);
    header.data_size = get_u32(bytes.data() + 14);
    header.checksum = get_u32(bytes.data() + 18);
    header.block_version = get_u64(bytes.data() + 22);

    if (header.magic != BLOCK_MAGIC || header.version != BLOCK_HEADER_VERSION) {
        return Result<BlockHeader>::err(ErrorCode::BLOCK_CORRUPTED);
    }
    return Result<BlockHeader>::ok(header);
}

BlockStorage::BlockStorage(std::filesystem::path blocks_dir, uint64_t capacity_bytes)
    : blocks_dir_(std::move(blocks_dir)), capacity_(capacity_bytes) {}

Result<std::unique_ptr<BlockStorage>> BlockStorage::create(std::filesystem::path directory,
                                                            uint64_t capacity_bytes) {
    std::filesystem::path blocks_dir = directory / "blocks";
    std::error_code ec;
    std::filesystem::create_directories(blocks_dir, ec);
    if (ec) {
        return Result<std::unique_ptr<BlockStorage>>::err(ErrorCode::INTERNAL_ERROR);
    }

    auto storage = std::unique_ptr<BlockStorage>(new BlockStorage(std::move(blocks_dir), capacity_bytes));
    ErrorCode recover_result = storage->recover();
    if (recover_result != ErrorCode::OK) {
        return Result<std::unique_ptr<BlockStorage>>::err(recover_result);
    }
    return Result<std::unique_ptr<BlockStorage>>::ok(std::move(storage));
}

std::filesystem::path BlockStorage::block_path(BlockId id) const {
    return blocks_dir_ / (std::to_string(id) + ".block");
}

std::filesystem::path BlockStorage::temp_path(BlockId id) const {
    return blocks_dir_ / (std::to_string(id) + ".block.tmp");
}

ErrorCode BlockStorage::recover() {
    // Runs once from create(), before any other thread has a reference to
    // this BlockStorage — no locking needed here.
    std::error_code ec;
    std::filesystem::directory_iterator it(blocks_dir_, ec);
    if (ec) {
        return ErrorCode::INTERNAL_ERROR;
    }

    for (const auto& entry : it) {
        if (!entry.is_regular_file()) continue;
        const std::filesystem::path& path = entry.path();

        if (path.extension() == ".tmp") {
            std::filesystem::remove(path, ec);
            continue;
        }
        if (path.extension() != ".block") continue;

        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;

        std::array<std::byte, BLOCK_HEADER_SIZE> header_bytes{};
        if (!full_pread(fd, header_bytes.data(), header_bytes.size(), 0)) {
            ::close(fd);
            continue;
        }

        auto header_result =
            deserialize_block_header(std::span<const std::byte, BLOCK_HEADER_SIZE>(header_bytes));
        if (header_result.is_error()) {
            ::close(fd);
            continue;
        }

        const BlockHeader& header = header_result.value();
        std::vector<uint8_t> data(header.data_size);
        bool data_ok = full_pread(fd, data.data(), data.size(), static_cast<off_t>(BLOCK_HEADER_SIZE));
        ::close(fd);
        if (!data_ok) continue;

        if (compute_block_checksum(data) != header.checksum) continue;  // corrupted, exclude from index

        auto state = std::make_shared<BlockState>();
        state->has_data = true;
        state->info = LocalBlockInfo{header.data_size, header.checksum, header.block_version};
        local_blocks_[header.block_id] = std::move(state);
        used_space_ += header.data_size;
    }

    return ErrorCode::OK;
}

std::shared_ptr<BlockStorage::BlockState> BlockStorage::find_block_state(BlockId id) const {
    std::shared_lock lock(map_mutex_);
    auto it = local_blocks_.find(id);
    if (it == local_blocks_.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<BlockStorage::BlockState> BlockStorage::get_or_create_block_state(BlockId id) {
    std::unique_lock lock(map_mutex_);
    auto it = local_blocks_.find(id);
    if (it != local_blocks_.end()) {
        return it->second;
    }
    auto state = std::make_shared<BlockState>();
    local_blocks_.emplace(id, state);
    return state;
}

ErrorCode BlockStorage::try_apply_space_delta(int64_t delta) {
    std::lock_guard<std::mutex> lock(space_mutex_);
    if (delta > 0) {
        uint64_t increase = static_cast<uint64_t>(delta);
        if (used_space_ + increase > capacity_) {
            return ErrorCode::STORAGE_FULL;
        }
        used_space_ += increase;
    } else if (delta < 0) {
        used_space_ -= static_cast<uint64_t>(-delta);
    }
    return ErrorCode::OK;
}

ErrorCode BlockStorage::write_block(BlockId id, const std::vector<uint8_t>& data,
                                     uint64_t block_version) {
    auto state = get_or_create_block_state(id);
    std::unique_lock lock(state->mutex);
    return write_block_locked(*state, id, data, block_version);
}

ErrorCode BlockStorage::write_block_locked(BlockState& state, BlockId id,
                                            const std::vector<uint8_t>& data,
                                            uint64_t block_version) {
    uint64_t existing_size = state.has_data ? state.info.size : 0;
    int64_t delta = static_cast<int64_t>(data.size()) - static_cast<int64_t>(existing_size);
    ErrorCode space_result = try_apply_space_delta(delta);
    if (space_result != ErrorCode::OK) {
        return space_result;
    }

    BlockHeader header{BLOCK_MAGIC,
                        BLOCK_HEADER_VERSION,
                        id,
                        static_cast<uint32_t>(data.size()),
                        compute_block_checksum(data),
                        block_version};
    auto header_bytes = serialize_block_header(header);

    std::filesystem::path tmp = temp_path(id);
    int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        try_apply_space_delta(-delta);
        return ErrorCode::INTERNAL_ERROR;
    }

    bool ok = full_pwrite(fd, header_bytes.data(), header_bytes.size(), 0) &&
              full_pwrite(fd, data.data(), data.size(), static_cast<off_t>(BLOCK_HEADER_SIZE));
    if (ok) {
        ok = (::fsync(fd) == 0);
    }
    ::close(fd);

    if (!ok) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        try_apply_space_delta(-delta);
        return ErrorCode::INTERNAL_ERROR;
    }

    std::error_code rename_ec;
    std::filesystem::rename(tmp, block_path(id), rename_ec);
    if (rename_ec) {
        std::filesystem::remove(tmp, rename_ec);
        try_apply_space_delta(-delta);
        return ErrorCode::INTERNAL_ERROR;
    }

    // Durability of rename() itself requires fsync'ing the containing
    // directory too — not mentioned in the design doc, added deliberately.
    int dir_fd = ::open(blocks_dir_.c_str(), O_RDONLY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }

    state.info = LocalBlockInfo{data.size(), header.checksum, block_version};
    state.has_data = true;

    return ErrorCode::OK;
}

Result<std::vector<uint8_t>> BlockStorage::read_block(BlockId id) {
    auto state = find_block_state(id);
    if (!state) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::BLOCK_NOT_FOUND);
    }
    std::shared_lock lock(state->mutex);
    return read_block_locked(*state, id);
}

Result<std::vector<uint8_t>> BlockStorage::read_block_locked(const BlockState& state, BlockId id) const {
    if (!state.has_data) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::BLOCK_NOT_FOUND);
    }

    int fd = ::open(block_path(id).c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::BLOCK_NOT_FOUND);
    }

    std::array<std::byte, BLOCK_HEADER_SIZE> header_bytes{};
    if (!full_pread(fd, header_bytes.data(), header_bytes.size(), 0)) {
        ::close(fd);
        return Result<std::vector<uint8_t>>::err(ErrorCode::BLOCK_CORRUPTED);
    }

    auto header_result =
        deserialize_block_header(std::span<const std::byte, BLOCK_HEADER_SIZE>(header_bytes));
    if (header_result.is_error()) {
        ::close(fd);
        return Result<std::vector<uint8_t>>::err(ErrorCode::BLOCK_CORRUPTED);
    }

    const BlockHeader& header = header_result.value();
    std::vector<uint8_t> data(header.data_size);
    bool data_ok = full_pread(fd, data.data(), data.size(), static_cast<off_t>(BLOCK_HEADER_SIZE));
    ::close(fd);

    if (!data_ok || header.block_id != id || compute_block_checksum(data) != header.checksum) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::BLOCK_CORRUPTED);
    }

    return Result<std::vector<uint8_t>>::ok(std::move(data));
}

ErrorCode BlockStorage::write_block_at(BlockId id, uint64_t offset,
                                        const std::vector<uint8_t>& data, uint64_t block_version) {
    auto state = find_block_state(id);
    if (!state) {
        return ErrorCode::BLOCK_NOT_FOUND;
    }

    std::unique_lock lock(state->mutex);
    if (!state->has_data) {
        return ErrorCode::BLOCK_NOT_FOUND;
    }
    if (offset > BLOCK_SIZE || data.size() > BLOCK_SIZE - offset) {
        return ErrorCode::INVALID_REQUEST;
    }

    auto current = read_block_locked(*state, id);
    if (current.is_error()) {
        return current.error();
    }
    std::vector<uint8_t> merged = std::move(current.value());
    if (merged.size() < offset + data.size()) {
        merged.resize(offset + data.size(), 0);  // Zero-fill any gap.
    }
    std::copy(data.begin(), data.end(), merged.begin() + static_cast<ptrdiff_t>(offset));

    return write_block_locked(*state, id, merged, block_version);
}

ErrorCode BlockStorage::delete_block(BlockId id) {
    // Deliberately does NOT erase the map entry — see the class-level
    // comment in block_storage.h. A BlockState, once created, is a
    // permanent handle for that id; find_block_state()/
    // get_or_create_block_state() will always return this same object, so
    // a write_block() that already resolved its shared_ptr before this
    // delete runs can never write into a state the map no longer knows
    // about.
    auto state = find_block_state(id);
    if (!state) {
        return ErrorCode::OK;  // idempotent — never touched
    }

    std::unique_lock lock(state->mutex);
    if (!state->has_data) {
        return ErrorCode::OK;  // idempotent — never had real data
    }

    std::error_code ec;
    std::filesystem::remove(block_path(id), ec);

    try_apply_space_delta(-static_cast<int64_t>(state->info.size));
    state->has_data = false;
    state->info = LocalBlockInfo{};

    return ErrorCode::OK;
}

bool BlockStorage::block_exists(BlockId id) const {
    auto state = find_block_state(id);
    if (!state) {
        return false;
    }
    std::shared_lock lock(state->mutex);
    return state->has_data;
}

Result<BlockStorage::BlockInfo> BlockStorage::get_block_info(BlockId id) const {
    auto state = find_block_state(id);
    if (!state) {
        return Result<BlockInfo>::err(ErrorCode::BLOCK_NOT_FOUND);
    }
    std::shared_lock lock(state->mutex);
    if (!state->has_data) {
        return Result<BlockInfo>::err(ErrorCode::BLOCK_NOT_FOUND);
    }
    return Result<BlockInfo>::ok(BlockInfo{state->info.size, state->info.checksum, state->info.version});
}

Result<BlockStorage::ConditionalReadResult> BlockStorage::read_block_if_changed(
    BlockId id, uint64_t known_version) {
    auto state = find_block_state(id);
    if (!state) {
        return Result<ConditionalReadResult>::err(ErrorCode::BLOCK_NOT_FOUND);
    }

    std::shared_lock lock(state->mutex);
    if (!state->has_data) {
        return Result<ConditionalReadResult>::err(ErrorCode::BLOCK_NOT_FOUND);
    }
    uint64_t current_version = state->info.version;
    if (known_version != 0 && known_version == current_version) {
        return Result<ConditionalReadResult>::ok(ConditionalReadResult{true, current_version, {}});
    }

    auto data_result = read_block_locked(*state, id);
    if (data_result.is_error()) {
        return Result<ConditionalReadResult>::err(data_result.error());
    }
    return Result<ConditionalReadResult>::ok(
        ConditionalReadResult{false, current_version, std::move(data_result.value())});
}

std::vector<std::pair<BlockId, BlockStorage::BlockInfo>> BlockStorage::enumerate_blocks() const {
    std::vector<std::pair<BlockId, std::shared_ptr<BlockState>>> snapshot;
    {
        std::shared_lock map_lock(map_mutex_);
        snapshot.reserve(local_blocks_.size());
        for (const auto& [id, state] : local_blocks_) {
            snapshot.emplace_back(id, state);
        }
    }

    std::vector<std::pair<BlockId, BlockInfo>> blocks;
    blocks.reserve(snapshot.size());
    for (const auto& [id, state] : snapshot) {
        std::shared_lock block_lock(state->mutex);
        if (!state->has_data) continue;
        blocks.emplace_back(id, BlockInfo{state->info.size, state->info.checksum, state->info.version});
    }
    return blocks;
}

uint64_t BlockStorage::used_space() const {
    std::lock_guard<std::mutex> lock(space_mutex_);
    return used_space_;
}

uint64_t BlockStorage::free_space() const {
    std::lock_guard<std::mutex> lock(space_mutex_);
    return capacity_ - used_space_;
}
