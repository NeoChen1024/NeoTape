#include "neotape/format.hpp"

#include <blake3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace neotape {

using std::size_t;
using std::string;
using std::string_view;

namespace {

constexpr size_t off_magic = 0;
constexpr size_t off_header_version = 8;
constexpr size_t off_channel_type = 9;
constexpr size_t off_volume_block_size_kib = 10;
constexpr size_t off_archive_uuid = 12;
constexpr size_t off_archive_label = 49;
constexpr size_t off_volume_seq_num = 114;
constexpr size_t off_global_frame_seq_num = 122;
constexpr size_t off_logical_slice_seq_num = 130;
constexpr size_t off_frame_seq_num_within_channel = 138;
constexpr size_t off_frame_payload_size = 146;
constexpr size_t off_flags = 150;
constexpr size_t off_reserved = 158;
constexpr size_t reserved_size = 250;
constexpr size_t off_signature = 408;
constexpr size_t off_frame_hash = 480;

void put_u16(HeaderBytes &bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xffU);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffU);
}

void put_u32(HeaderBytes &bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffU);
    }
}

void put_u64(HeaderBytes &bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffU);
    }
}

uint16_t get_u16(const uint8_t *bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1]) << 8;
}

uint32_t get_u32(const uint8_t *bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

uint64_t get_u64(const uint8_t *bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

void put_fixed_string(HeaderBytes &bytes, size_t offset, size_t field_size,
                      string_view value) {
    const size_t n = std::min(value.size(), field_size - 1);
    std::memcpy(bytes.data() + offset, value.data(), n);
    std::memset(bytes.data() + offset + n, 0, field_size - n);
}

string get_fixed_string(const uint8_t *bytes, size_t offset,
                        size_t field_size) {
    if (bytes[offset + field_size - 1] != 0) {
        throw std::runtime_error("fixed string field without trailing NUL");
    }

    const auto *begin = reinterpret_cast<const char *>(bytes + offset);
    const char *end = std::find(begin, begin + field_size, '\0');
    return string(begin, end);
}

ChannelType get_channel_type(uint8_t value) {
    switch (value) {
    case static_cast<uint8_t>(ChannelType::CH_CONTENT):
        return ChannelType::CH_CONTENT;
    case static_cast<uint8_t>(ChannelType::CH_METADATA):
        return ChannelType::CH_METADATA;
    case static_cast<uint8_t>(ChannelType::ARCHIVE_END):
        return ChannelType::ARCHIVE_END;
    default:
        throw std::runtime_error("unsupported channel type");
    }
}

void validate_reserved(const uint8_t *data) {
    for (size_t i = off_reserved; i < off_reserved + reserved_size; ++i) {
        if (data[i] != 0) {
            throw std::runtime_error("reserved header bytes must be zero");
        }
    }
}

bool has_nonzero_signature(const SignatureBytes &signature) {
    return std::ranges::any_of(signature,
                               [](uint8_t byte) { return byte != 0; });
}

void validate_serialized_signature(const FrameHeader &header) {
    if (!has_frame_flag_signed(header.flags) &&
        has_nonzero_signature(header.signature)) {
        throw std::runtime_error("unsigned frame cannot carry signature bytes");
    }
}

} // namespace

uint32_t decoded_block_size(const FrameHeader &header) {
    return static_cast<uint32_t>(header.volume_block_size_kib) * 1024U;
}

bool valid_block_size(uint32_t block_size) {
    return block_size >= min_block_size && block_size <= max_block_size &&
           block_size % 1024U == 0;
}

HeaderBytes serialize_frame_header(const FrameHeader &header) {
    validate_serialized_signature(header);

    HeaderBytes bytes{};

    std::transform(magic.begin(), magic.end(), bytes.begin(),
                   [](char c) { return static_cast<uint8_t>(c); });
    bytes[off_header_version] = header_version;
    bytes[off_channel_type] = static_cast<uint8_t>(header.channel_type);
    put_u16(bytes, off_volume_block_size_kib, header.volume_block_size_kib);
    put_fixed_string(bytes, off_archive_uuid, nt_uuid_size,
                     header.archive_uuid);
    put_fixed_string(bytes, off_archive_label, archive_label_size,
                     header.archive_label);
    put_u64(bytes, off_volume_seq_num, header.volume_seq_num);
    put_u64(bytes, off_global_frame_seq_num, header.global_frame_seq_num);
    put_u64(bytes, off_logical_slice_seq_num, header.logical_slice_seq_num);
    put_u64(bytes, off_frame_seq_num_within_channel,
            header.frame_seq_num_within_channel);
    put_u32(bytes, off_frame_payload_size, header.frame_payload_size);
    put_u64(bytes, off_flags, header.flags);
    std::copy(header.signature.begin(), header.signature.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(off_signature));
    std::copy(header.frame_hash.begin(), header.frame_hash.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(off_frame_hash));

    parse_fixed_header(bytes.data(), bytes.size());
    return bytes;
}

FrameHeader parse_frame_header(const uint8_t *data, std::size_t size) {
    if (size < fixed_header_size) {
        throw std::runtime_error("short fixed header");
    }
    if (std::memcmp(data + off_magic, magic.data(), magic.size()) != 0) {
        throw std::runtime_error("bad magic");
    }
    if (data[off_header_version] != header_version) {
        throw std::runtime_error(std::format("unsupported header version {}",
                                             data[off_header_version]));
    }

    validate_reserved(data);

    FrameHeader header;
    header.channel_type = get_channel_type(data[off_channel_type]);
    header.volume_block_size_kib = get_u16(data, off_volume_block_size_kib);
    header.archive_uuid =
        get_fixed_string(data, off_archive_uuid, nt_uuid_size);
    header.archive_label =
        get_fixed_string(data, off_archive_label, archive_label_size);
    header.volume_seq_num = get_u64(data, off_volume_seq_num);
    header.global_frame_seq_num = get_u64(data, off_global_frame_seq_num);
    header.logical_slice_seq_num = get_u64(data, off_logical_slice_seq_num);
    header.frame_seq_num_within_channel =
        get_u64(data, off_frame_seq_num_within_channel);
    header.frame_payload_size = get_u32(data, off_frame_payload_size);
    header.flags = get_u64(data, off_flags);
    std::copy(data + off_signature, data + off_signature + signature_size,
              header.signature.begin());
    std::copy(data + off_frame_hash,
              data + off_frame_hash + header.frame_hash.size(),
              header.frame_hash.begin());

    neotape::validate_header(header);
    return header;
}

FrameHeader parse_fixed_header(const uint8_t *data, std::size_t size) {
    return parse_frame_header(data, size);
}

std::string channel_type_name(ChannelType type) {
    switch (type) {
    case ChannelType::CH_CONTENT:
        return "CH_CONTENT";
    case ChannelType::CH_METADATA:
        return "CH_METADATA";
    case ChannelType::ARCHIVE_END:
        return "ARCHIVE_END";
    }
    return "unknown";
}

std::string hash_hex(const Hash &hash) {
    std::string hex;
    for (uint8_t byte : hash) {
        hex += std::format("{:02x}", static_cast<unsigned>(byte));
    }
    return hex;
}

Hash blake3_hash(const uint8_t *data, std::size_t size) {
    Hash hash{};
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, size);
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());
    return hash;
}

Hash compute_frame_hash(const uint8_t *data, std::size_t size) {
    FrameHeader const header = parse_fixed_header(data, size);
    if (size != decoded_block_size(header)) {
        throw std::runtime_error(
            "record size does not match decoded block size");
    }

    std::vector<uint8_t> canonical(data, data + size);
    for (size_t i = off_signature; i < fixed_header_size; ++i) {
        canonical[i] = 0;
    }
    return blake3_hash(canonical.data(), canonical.size());
}

std::string utc_timestamp_now() {
    std::time_t now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    if (gmtime_r(&now, &utc) == nullptr) {
        throw std::runtime_error("gmtime_r failed");
    }

    std::array<char, 20> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S",
                      &utc) != 19) {
        throw std::runtime_error("strftime failed");
    }
    return std::string(buffer.data(), 19);
}

std::string make_uuid_v4() {
    std::array<uint8_t, 16> bytes{};
    std::random_device rd;
    for (uint8_t &byte : bytes) {
        byte = static_cast<uint8_t>(rd());
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    return std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
                       "{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
                       bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                       bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                       bytes[15]);
}

void validate_header(const FrameHeader &header) {
    const uint32_t block_size = decoded_block_size(header);
    if (!valid_block_size(block_size)) {
        throw std::runtime_error("invalid volume block size");
    }
    if (header.frame_payload_size > block_size - fixed_header_size) {
        throw std::runtime_error(
            "frame payload size exceeds block payload capacity");
    }

    constexpr uint64_t allowed_flags = frame_flag_start | frame_flag_end |
                                       frame_flag_signed | frame_flag_clean_end;
    if ((header.flags & ~allowed_flags) != 0) {
        throw std::runtime_error("reserved frame flag bits set");
    }

    if (header.channel_type == ChannelType::ARCHIVE_END) {
        if (!has_frame_flag_start(header.flags) ||
            !has_frame_flag_end(header.flags) ||
            !has_frame_flag_clean_end(header.flags)) {
            throw std::runtime_error(
                "archive-end frame missing required flags");
        }
        if (header.logical_slice_seq_num != 0) {
            throw std::runtime_error(
                "archive-end frame has logical slice sequence");
        }
        if (header.frame_seq_num_within_channel != 1) {
            throw std::runtime_error(
                "archive-end frame channel sequence must be one");
        }
    } else if (has_frame_flag_clean_end(header.flags)) {
        throw std::runtime_error(
            "CLEAN_END is only valid on archive-end frames");
    }
}

bool verify_frame_hash(const uint8_t *data, std::size_t size,
                       const Hash &expected) {
    return compute_frame_hash(data, size) == expected;
}

} // namespace neotape
