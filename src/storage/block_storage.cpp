#include "storage/block_storage.h"

#include <endian.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <mutex>
#include <system_error>

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

// Standard CRC-32 (IEEE 802.3 polynomial 0xEDB88320), table-based.
uint32_t crc32(const std::vector<uint8_t>& data) {
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

std::array<std::byte, BLOCK_HEADER_SIZE> serialize_block_header(const BlockHeader& header) {
    std::array<std::byte, BLOCK_HEADER_SIZE> bytes{};
    put_u32(bytes.data(), header.magic);
    put_u16(bytes.data() + 4, header.version);
    put_u64(bytes.data() + 6, header.block_id);
    put_u32(bytes.data() + 14, header.data_size);
    put_u32(bytes.data() + 18, header.checksum);
    return bytes;
}

Result<BlockHeader> deserialize_block_header(std::span<const std::byte, BLOCK_HEADER_SIZE> bytes) {
    BlockHeader header{};
    header.magic = get_u32(bytes.data());
    header.version = get_u16(bytes.data() + 4);
    header.block_id = get_u64(bytes.data() + 6);
    header.data_size = get_u32(bytes.data() + 14);
    header.checksum = get_u32(bytes.data() + 18);

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

        if (crc32(data) != header.checksum) continue;  // corrupted, exclude from index

        local_blocks_[header.block_id] = LocalBlockInfo{header.data_size, header.checksum};
        used_space_ += header.data_size;
    }

    return ErrorCode::OK;
}

ErrorCode BlockStorage::write_block(BlockId id, const std::vector<uint8_t>& data) {
    std::unique_lock lock(mutex_);

    uint64_t existing_size = 0;
    if (auto it = local_blocks_.find(id); it != local_blocks_.end()) {
        existing_size = it->second.size;
    }
    uint64_t prospective_used = used_space_ - existing_size + data.size();
    if (prospective_used > capacity_) {
        return ErrorCode::STORAGE_FULL;
    }

    BlockHeader header{BLOCK_MAGIC, BLOCK_HEADER_VERSION, id, static_cast<uint32_t>(data.size()),
                        crc32(data)};
    auto header_bytes = serialize_block_header(header);

    std::filesystem::path tmp = temp_path(id);
    int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
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
        return ErrorCode::INTERNAL_ERROR;
    }

    std::error_code rename_ec;
    std::filesystem::rename(tmp, block_path(id), rename_ec);
    if (rename_ec) {
        std::filesystem::remove(tmp, rename_ec);
        return ErrorCode::INTERNAL_ERROR;
    }

    // Durability of rename() itself requires fsync'ing the containing
    // directory too — not mentioned in the design doc, added deliberately.
    int dir_fd = ::open(blocks_dir_.c_str(), O_RDONLY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }

    local_blocks_[id] = LocalBlockInfo{data.size(), header.checksum};
    used_space_ = prospective_used;

    return ErrorCode::OK;
}

Result<std::vector<uint8_t>> BlockStorage::read_block(BlockId id) {
    std::shared_lock lock(mutex_);

    if (local_blocks_.find(id) == local_blocks_.end()) {
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

    if (!data_ok || header.block_id != id || crc32(data) != header.checksum) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::BLOCK_CORRUPTED);
    }

    return Result<std::vector<uint8_t>>::ok(std::move(data));
}

ErrorCode BlockStorage::delete_block(BlockId id) {
    std::unique_lock lock(mutex_);

    auto it = local_blocks_.find(id);
    if (it == local_blocks_.end()) {
        return ErrorCode::OK;  // idempotent
    }

    std::error_code ec;
    std::filesystem::remove(block_path(id), ec);

    used_space_ -= it->second.size;
    local_blocks_.erase(it);

    return ErrorCode::OK;
}

bool BlockStorage::block_exists(BlockId id) const {
    std::shared_lock lock(mutex_);
    return local_blocks_.contains(id);
}

uint64_t BlockStorage::used_space() const {
    std::shared_lock lock(mutex_);
    return used_space_;
}

uint64_t BlockStorage::free_space() const {
    std::shared_lock lock(mutex_);
    return capacity_ - used_space_;
}
