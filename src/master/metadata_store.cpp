#include "master/metadata_store.h"

#include <endian.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <system_error>
#include <utility>

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

void put_u8(std::vector<std::byte>& buf, uint8_t value) { buf.push_back(static_cast<std::byte>(value)); }

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

constexpr uint32_t kMetadataMagic = 0x4D455453;  // "METS"
constexpr uint16_t kMetadataFormatVersion = 1;

// Same CRC-32 (IEEE 802.3, 0xEDB88320) as storage/block_header.h's
// compute_block_checksum() — reimplemented locally rather than pulling in
// a dependency on the storage library, matching this project's existing
// convention of duplicating small wire-format helpers per module (see
// put_u16/get_u16 above, already duplicated in master.cpp, client.cpp,
// storage_node.cpp, protocol.cpp).
uint32_t compute_crc32(const std::byte* data, size_t size) {
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
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFFu] ^ (crc >> 8);
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

// Bounds-checked cursor over the loaded snapshot bytes. Every accessor
// checks remaining size before reading; once a violation is hit, `ok`
// latches false and every subsequent read is a safe no-op (never reads out
// of bounds) — callers still check `ok` once at the very end rather than
// after every single call, but loops break out early on `!ok` to avoid
// spinning through a bogus huge count field doing nothing (see
// docs/design.md §16).
struct Reader {
    const std::byte* data;
    size_t size;
    size_t offset = 0;
    bool ok = true;

    bool has(size_t n) const { return ok && offset + n <= size; }

    uint32_t u32() {
        if (!has(sizeof(uint32_t))) {
            ok = false;
            return 0;
        }
        uint32_t v = get_u32(data + offset);
        offset += sizeof(uint32_t);
        return v;
    }
    uint64_t u64() {
        if (!has(sizeof(uint64_t))) {
            ok = false;
            return 0;
        }
        uint64_t v = get_u64(data + offset);
        offset += sizeof(uint64_t);
        return v;
    }
    uint16_t u16() {
        if (!has(sizeof(uint16_t))) {
            ok = false;
            return 0;
        }
        uint16_t v = get_u16(data + offset);
        offset += sizeof(uint16_t);
        return v;
    }
    uint8_t u8() {
        if (!has(1)) {
            ok = false;
            return 0;
        }
        uint8_t v = static_cast<uint8_t>(data[offset]);
        offset += 1;
        return v;
    }
    std::string str() {
        uint32_t len = u32();
        if (!has(len)) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data + offset), len);
        offset += len;
        return s;
    }
};

}  // namespace

MetadataStore::MetadataStore(std::filesystem::path data_dir) : data_dir_(std::move(data_dir)) {}

Result<std::unique_ptr<MetadataStore>> MetadataStore::create(std::filesystem::path data_dir) {
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    if (ec) {
        return Result<std::unique_ptr<MetadataStore>>::err(ErrorCode::INTERNAL_ERROR);
    }
    auto store = std::unique_ptr<MetadataStore>(new MetadataStore(std::move(data_dir)));
    return Result<std::unique_ptr<MetadataStore>>::ok(std::move(store));
}

std::filesystem::path MetadataStore::snapshot_path() const { return data_dir_ / "metadata.snapshot"; }

std::filesystem::path MetadataStore::temp_path() const { return data_dir_ / "metadata.snapshot.tmp"; }

ErrorCode MetadataStore::save(const std::vector<FileMetadata>& files,
                               const std::vector<std::pair<BlockId, BlockMetadata>>& blocks,
                               uint64_t next_file_id, uint64_t next_block_id,
                               const std::vector<StorageNodeInfo>& nodes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Wire format: [u32 magic][u16 format_version][u64 next_file_id]
    // [u64 next_block_id][u32 file_count]{[u64 file_id][u32 path_len][path]
    // [u64 size][u64 mtime][u32 block_count]{u64}xN} x file_count
    // [u32 block_count]{[u64 block_id][u64 size][u64 reserved_size]
    // [u64 version][u8 state][u32 replica_count]{u64}xN} x block_count
    // [u32 node_count]{[u64 node_id][u32 host_len][host][u16 port]
    // [u64 total_space][u64 free_space]} x node_count [u32 crc32-of-everything-above]
    // (see docs/design.md §16).
    std::vector<std::byte> buf;
    put_u32(buf, kMetadataMagic);
    put_u16(buf, kMetadataFormatVersion);
    put_u64(buf, next_file_id);
    put_u64(buf, next_block_id);

    put_u32(buf, static_cast<uint32_t>(files.size()));
    for (const FileMetadata& file : files) {
        put_u64(buf, file.id);
        put_string(buf, file.path);
        put_u64(buf, file.size);
        put_u64(buf, file.modification_time);
        put_u32(buf, static_cast<uint32_t>(file.blocks.size()));
        for (BlockId block_id : file.blocks) {
            put_u64(buf, block_id);
        }
    }

    put_u32(buf, static_cast<uint32_t>(blocks.size()));
    for (const auto& [block_id, metadata] : blocks) {
        put_u64(buf, block_id);
        put_u64(buf, metadata.size);
        put_u64(buf, metadata.reserved_size);
        put_u64(buf, metadata.version);
        put_u8(buf, static_cast<uint8_t>(metadata.state));
        put_u32(buf, static_cast<uint32_t>(metadata.replicas.size()));
        for (StorageNodeId replica : metadata.replicas) {
            put_u64(buf, replica);
        }
    }

    put_u32(buf, static_cast<uint32_t>(nodes.size()));
    for (const StorageNodeInfo& node : nodes) {
        put_u64(buf, node.id);
        put_string(buf, node.host);
        put_u16(buf, node.port);
        put_u64(buf, node.total_space);
        put_u64(buf, node.free_space);
    }

    put_u32(buf, compute_crc32(buf.data(), buf.size()));

    std::filesystem::path tmp = temp_path();
    int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        return ErrorCode::INTERNAL_ERROR;
    }
    bool ok = full_pwrite(fd, buf.data(), buf.size(), 0);
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
    std::filesystem::rename(tmp, snapshot_path(), rename_ec);
    if (rename_ec) {
        std::filesystem::remove(tmp, rename_ec);
        return ErrorCode::INTERNAL_ERROR;
    }

    // Durability of rename() itself requires fsync'ing the containing
    // directory too — same reasoning as BlockStorage::write_block_locked().
    int dir_fd = ::open(data_dir_.c_str(), O_RDONLY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return ErrorCode::OK;
}

Result<MetadataStore::LoadedState> MetadataStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);

    // A stray .tmp left over from a crash mid-write is never partially
    // "valid" data to salvage — the canonical file only ever changes via
    // an atomic rename — so just discard it, same as
    // BlockStorage::recover() discarding stray *.block.tmp files.
    std::error_code remove_ec;
    std::filesystem::remove(temp_path(), remove_ec);

    LoadedState state;

    std::error_code exists_ec;
    if (!std::filesystem::exists(snapshot_path(), exists_ec)) {
        return Result<LoadedState>::ok(std::move(state));  // fresh cluster
    }

    int fd = ::open(snapshot_path().c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<LoadedState>::ok(std::move(state));  // unreadable — treat as fresh, don't refuse to boot
    }
    off_t file_size = ::lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        ::close(fd);
        return Result<LoadedState>::ok(std::move(state));
    }
    std::vector<std::byte> buf(static_cast<size_t>(file_size));
    bool read_ok = full_pread(fd, buf.data(), buf.size(), 0);
    ::close(fd);
    if (!read_ok) {
        return Result<LoadedState>::ok(std::move(state));  // truncated/corrupted — treat as fresh
    }

    if (buf.size() < sizeof(uint32_t)) {
        return Result<LoadedState>::ok(std::move(state));  // too small to even hold a trailing CRC
    }
    size_t crc_offset = buf.size() - sizeof(uint32_t);
    uint32_t stored_crc = get_u32(buf.data() + crc_offset);
    if (compute_crc32(buf.data(), crc_offset) != stored_crc) {
        return Result<LoadedState>::ok(std::move(state));  // corrupted — treat as fresh, don't refuse to boot
    }

    Reader reader{buf.data(), crc_offset};
    uint32_t magic = reader.u32();
    uint16_t format_version = reader.u16();
    if (!reader.ok || magic != kMetadataMagic || format_version != kMetadataFormatVersion) {
        return Result<LoadedState>::ok(std::move(state));  // unrecognized — treat as fresh
    }

    state.next_file_id = reader.u64();
    state.next_block_id = reader.u64();

    uint32_t file_count = reader.u32();
    state.files.reserve(file_count);
    for (uint32_t i = 0; i < file_count && reader.ok; ++i) {
        FileMetadata file{};
        file.id = reader.u64();
        file.path = reader.str();
        file.size = reader.u64();
        file.modification_time = reader.u64();
        uint32_t block_count = reader.u32();
        file.blocks.reserve(block_count);
        for (uint32_t b = 0; b < block_count && reader.ok; ++b) {
            file.blocks.push_back(reader.u64());
        }
        if (!reader.ok) break;
        state.files.push_back(std::move(file));
    }

    uint32_t block_count = reader.ok ? reader.u32() : 0;
    state.blocks.reserve(block_count);
    for (uint32_t i = 0; i < block_count && reader.ok; ++i) {
        BlockId block_id = reader.u64();
        BlockMetadata metadata{};
        metadata.id = block_id;
        metadata.size = reader.u64();
        metadata.reserved_size = reader.u64();
        metadata.version = reader.u64();
        metadata.state = static_cast<BlockState>(reader.u8());
        uint32_t replica_count = reader.u32();
        metadata.replicas.reserve(replica_count);
        for (uint32_t r = 0; r < replica_count && reader.ok; ++r) {
            metadata.replicas.push_back(reader.u64());
        }
        if (!reader.ok) break;
        state.blocks.emplace_back(block_id, std::move(metadata));
    }

    uint32_t node_count = reader.ok ? reader.u32() : 0;
    state.nodes.reserve(node_count);
    for (uint32_t i = 0; i < node_count && reader.ok; ++i) {
        StorageNodeInfo node{};
        node.id = reader.u64();
        node.host = reader.str();
        node.port = reader.u16();
        node.total_space = reader.u64();
        node.free_space = reader.u64();
        if (!reader.ok) break;
        state.nodes.push_back(std::move(node));
    }

    if (!reader.ok) {
        return Result<LoadedState>::ok(LoadedState{});  // malformed midway — discard the partial parse
    }
    return Result<LoadedState>::ok(std::move(state));
}
