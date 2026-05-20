// auto-generated — do not edit
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <format>
#include <crc32c/crc32c.h>

namespace neotape {

inline constexpr std::size_t fixed_header_size = 1024;
inline constexpr std::array<char, 8> magic = {'N','e','o','T','a','p','e','\0'};
inline constexpr uint8_t header_version = 1;
inline constexpr uint32_t min_block_size = 4096;
inline constexpr uint32_t max_block_size = 16 * 1024 * 1024;

using HeaderBytes = std::array<uint8_t, fixed_header_size>;
using Hash = std::array<uint8_t, 32>;

// Fixed string field sizes
inline constexpr std::size_t nt_uuid_size = 37;
inline constexpr std::size_t nt_name_size = 256;
inline constexpr std::size_t nt_time_size = 20;
inline constexpr std::size_t ident64_size = 64;

enum class HeaderType : uint8_t {
    medium = 1,
    volume = 2,
    frame = 3,
    archive_end = 4,
};

enum class PayloadProfile : uint8_t {
    raw = 1,
    pax = 2,
};

enum class FrameContentType : uint8_t {
    slice_content = 1,
    slice_metadata = 2,
};

inline constexpr uint16_t frame_flag_start = 1u << 0;
inline constexpr uint16_t frame_flag_end = 1u << 1;

inline constexpr bool has_frame_flag_start(uint16_t f) { return f & frame_flag_start; }
inline constexpr bool has_frame_flag_end(uint16_t f) { return f & frame_flag_end; }

inline constexpr uint16_t archive_end_flag_clean_end = 1u << 0;
inline constexpr uint16_t archive_end_flag_catalog_present = 1u << 1;

inline constexpr bool has_archive_end_flag_clean_end(uint16_t f) { return f & archive_end_flag_clean_end; }
inline constexpr bool has_archive_end_flag_catalog_present(uint16_t f) { return f & archive_end_flag_catalog_present; }

// Common prefix offsets
inline constexpr std::size_t com_header_version = 8;
inline constexpr std::size_t com_header_type = 9;

// CRC32C always at the last 4 bytes of the fixed header
inline constexpr std::size_t hdr_crc32c = 1020;

// Shared identity block offsets
inline constexpr std::size_t hdr_volume_block_size = 10;
inline constexpr std::size_t hdr_archive_uuid = 14;
inline constexpr std::size_t hdr_archive_name = 51;
inline constexpr std::size_t hdr_volume_seq_num = 307;
inline constexpr std::size_t hdr_payload_profile = 315;

// Type-specific offsets
// VolumeHeader
inline constexpr std::size_t vhdr_volume_write_at_utc = 316;
inline constexpr std::size_t vhdr_flags = 336;
// FrameHeader
inline constexpr std::size_t fhdr_logical_slice_seq_num = 316;
inline constexpr std::size_t fhdr_global_frame_seq_num = 324;
inline constexpr std::size_t fhdr_frame_seq_num_within_slice = 332;
inline constexpr std::size_t fhdr_frame_payload_size = 340;
inline constexpr std::size_t fhdr_frame_content_type = 348;
inline constexpr std::size_t fhdr_frame_payload_blake3 = 349;
inline constexpr std::size_t fhdr_flags = 381;
inline constexpr std::size_t fhdr_slice_content_size = 383;
inline constexpr std::size_t fhdr_slice_content_blake3 = 391;
// MediumHeader
inline constexpr std::size_t mhdr_medium_header_block_size = 10;
inline constexpr std::size_t mhdr_medium_uuid = 14;
inline constexpr std::size_t mhdr_medium_label = 51;
inline constexpr std::size_t mhdr_initialized_at_utc = 307;
inline constexpr std::size_t mhdr_medium_header_block_count = 327;
inline constexpr std::size_t mhdr_flags = 329;
inline constexpr std::size_t mhdr_created_by_implementation = 331;
inline constexpr std::size_t mhdr_created_by_build_id = 395;
inline constexpr std::size_t mhdr_metadata_bundle_size = 459;
inline constexpr std::size_t mhdr_metadata_bundle_blake3 = 463;
// ArchiveEndHeader
inline constexpr std::size_t ae_last_logical_slice_seq_num = 316;
inline constexpr std::size_t ae_last_global_frame_seq_num = 324;
inline constexpr std::size_t ae_created_by_implementation = 332;
inline constexpr std::size_t ae_created_by_build_id = 396;
inline constexpr std::size_t ae_archive_end_at_utc = 460;
inline constexpr std::size_t ae_flags = 480;
struct VolumeHeader {
    uint32_t volume_block_size{0};
    std::string archive_uuid{};
    std::string archive_name{};
    uint64_t volume_seq_num{0};
    PayloadProfile payload_profile{PayloadProfile::raw};
    std::string volume_write_at_utc{};
    uint16_t flags{0};
};

struct FrameHeader {
    uint32_t volume_block_size{0};
    std::string archive_uuid{};
    std::string archive_name{};
    uint64_t volume_seq_num{0};
    PayloadProfile payload_profile{PayloadProfile::raw};
    uint64_t logical_slice_seq_num{0};
    uint64_t global_frame_seq_num{0};
    uint64_t frame_seq_num_within_slice{0};
    uint64_t frame_payload_size{0};
    FrameContentType frame_content_type{FrameContentType::slice_content};
    Hash frame_payload_blake3{Hash{}};
    uint16_t flags{0};
    uint64_t slice_content_size{0};
    Hash slice_content_blake3{Hash{}};
};

struct MediumHeader {
    uint32_t medium_header_block_size{0};
    std::string medium_uuid{};
    std::string medium_label{};
    std::string initialized_at_utc{};
    uint16_t medium_header_block_count{0};
    uint16_t flags{0};
    std::string created_by_implementation{};
    std::string created_by_build_id{};
    uint32_t metadata_bundle_size{0};
    Hash metadata_bundle_blake3{Hash{}};
};

struct ArchiveEndHeader {
    uint32_t volume_block_size{0};
    std::string archive_uuid{};
    std::string archive_name{};
    uint64_t volume_seq_num{0};
    PayloadProfile payload_profile{PayloadProfile::raw};
    uint64_t last_logical_slice_seq_num{0};
    uint64_t last_global_frame_seq_num{0};
    std::string created_by_implementation{};
    std::string created_by_build_id{};
    std::string archive_end_at_utc{};
    uint16_t flags{archive_end_flag_clean_end};
};

namespace detail {

// ── Integer helpers ──
inline void put_u16(HeaderBytes &bytes, std::size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xffu);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

inline void put_u32(HeaderBytes &bytes, std::size_t offset, uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);
}

inline void put_u64(HeaderBytes &bytes, std::size_t offset, uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i)
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);
}

inline uint16_t get_u16(const uint8_t *bytes, std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1]) << 8;
}

inline uint32_t get_u32(const uint8_t *bytes, std::size_t offset) {
    uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
    return value;
}

inline uint64_t get_u64(const uint8_t *bytes, std::size_t offset) {
    uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
    return value;
}

// ── Bytes / Hash helpers ──
inline void put_bytes(HeaderBytes &bytes, std::size_t offset, const Hash &hash) {
    std::ranges::copy(hash, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

inline Hash get_hash(const uint8_t *bytes, std::size_t offset) {
    Hash hash{};
    std::copy(bytes + offset, bytes + offset + hash.size(), hash.begin());
    return hash;
}

// ── String helpers ──
inline void put_fixed_string(HeaderBytes &bytes, std::size_t offset, std::size_t size, std::string_view value) {
    std::size_t n = std::min(value.size(), size - 1);
    std::memcpy(bytes.data() + offset, value.data(), n);
    std::memset(bytes.data() + offset + n, 0, size - n);
}

inline std::string get_fixed_string(const uint8_t *bytes, std::size_t offset, std::size_t size) {
    const auto *begin = reinterpret_cast<const char *>(bytes + offset);
    const char *end = std::find(begin, begin + size, '\0');
    return std::string(begin, end);
}

inline std::string get_nt_name(const uint8_t *bytes, std::size_t offset, std::size_t size) {
    if (bytes[offset + size - 1] != 0)
        throw std::runtime_error("nt_name field without trailing NUL");
    return get_fixed_string(bytes, offset, size);
}

} // namespace detail

// ── Header construction and CRC ──
inline HeaderBytes make_header(HeaderType type) {
    HeaderBytes bytes{};
    for (std::size_t i = 0; i < magic.size(); ++i)
        bytes[i] = static_cast<uint8_t>(magic[i]);
    bytes[com_header_version] = header_version;
    bytes[com_header_type] = static_cast<uint8_t>(type);
    return bytes;
}

inline void finish_crc(HeaderBytes &bytes) {
    uint32_t crc = crc32c::Crc32c(bytes.data(), fixed_header_size - 4);
    detail::put_u32(bytes, hdr_crc32c, crc);
}

// ── Common prefix validation ──
inline void check_common(const uint8_t *data, std::size_t size) {
    if (size < fixed_header_size)
        throw std::runtime_error("short fixed header");
    for (std::size_t i = 0; i < magic.size(); ++i)
        if (data[i] != static_cast<uint8_t>(magic[i]))
            throw std::runtime_error("bad magic");
    if (data[com_header_version] != header_version)
        throw std::runtime_error(std::format("unsupported header version {}", data[8]));
}


// Parser declarations (implemented in generated .cpp)
VolumeHeader parse_volume(const uint8_t *data);
FrameHeader parse_frame(const uint8_t *data);
MediumHeader parse_medium(const uint8_t *data);
ArchiveEndHeader parse_archive_end(const uint8_t *data);

} // namespace neotape