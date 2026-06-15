#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace neotape {

inline constexpr std::size_t fixed_header_size = 512;
inline constexpr std::array<char, 8> magic = {'N', 'e', 'o', 'T',
                                              'a', 'p', 'e', '\0'};
inline constexpr uint8_t header_version = 1;
inline constexpr uint32_t min_block_size = 4096;
inline constexpr uint32_t max_block_size = 8 * 1024 * 1024;
inline constexpr std::size_t nt_uuid_size = 37;
inline constexpr std::size_t archive_label_size = 65;
inline constexpr std::size_t signature_size = 72;

using HeaderBytes = std::array<uint8_t, fixed_header_size>;
using Hash = std::array<uint8_t, 32>;
using SignatureBytes = std::array<uint8_t, signature_size>;

enum class ChannelType : uint8_t {
    CH_CONTENT = 1,
    CH_METADATA = 2,
    ARCHIVE_END = 255,
};

inline constexpr uint64_t frame_flag_start = 1ull << 0;
inline constexpr uint64_t frame_flag_end = 1ull << 1;
inline constexpr uint64_t frame_flag_signed = 1ull << 2;
inline constexpr uint64_t frame_flag_clean_end = 1ull << 63;

constexpr bool has_frame_flag_start(uint64_t flags) {
    return (flags & frame_flag_start) != 0;
}
constexpr bool has_frame_flag_end(uint64_t flags) {
    return (flags & frame_flag_end) != 0;
}
constexpr bool has_frame_flag_signed(uint64_t flags) {
    return (flags & frame_flag_signed) != 0;
}
constexpr bool has_frame_flag_clean_end(uint64_t flags) {
    return (flags & frame_flag_clean_end) != 0;
}

struct FrameHeader {
    ChannelType channel_type{ChannelType::CH_CONTENT};
    uint16_t volume_block_size_kib{0};
    std::string archive_uuid;
    std::string archive_label;
    uint64_t volume_seq_num{0};
    uint64_t global_frame_seq_num{0};
    uint64_t logical_slice_seq_num{0};
    uint64_t frame_seq_num_within_channel{0};
    uint32_t frame_payload_size{0};
    uint64_t flags{0};
    SignatureBytes signature{};
    Hash frame_hash{};
};

HeaderBytes serialize_frame_header(const FrameHeader &header);
FrameHeader parse_frame_header(const uint8_t *data, std::size_t size);
FrameHeader parse_fixed_header(const uint8_t *data, std::size_t size);

std::string channel_type_name(ChannelType type);
std::string hash_hex(const Hash &hash);
Hash blake3_hash(const uint8_t *data, std::size_t size);
Hash compute_frame_hash(const uint8_t *data, std::size_t size);
uint32_t decoded_block_size(const FrameHeader &header);
bool valid_block_size(uint32_t block_size);
std::string utc_timestamp_now();
std::string make_uuid_v4();

} // namespace neotape
