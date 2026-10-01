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
inline constexpr std::size_t sideband_size = 128;

using HeaderBytes = std::array<uint8_t, fixed_header_size>;
using Hash = std::array<uint8_t, 32>;
using SignatureBytes = std::array<uint8_t, signature_size>;
using SidebandBytes = std::array<uint8_t, sideband_size>;

enum class ChannelType : uint8_t {
    CH_CONTENT = 1,
    CH_METADATA = 2,
    ARCHIVE_END = 255,
};

inline constexpr uint64_t frame_flag_end = 1ULL << 0;
inline constexpr uint64_t frame_flag_signed = 1ULL << 1;
inline constexpr uint64_t frame_flag_sideband = 1ULL << 2;
inline constexpr uint64_t frame_flag_clean_end = 1ULL << 63;

constexpr bool has_frame_flag_end(uint64_t flags) {
    return (flags & frame_flag_end) != 0;
}
constexpr bool has_frame_flag_signed(uint64_t flags) {
    return (flags & frame_flag_signed) != 0;
}
constexpr bool has_frame_flag_sideband(uint64_t flags) {
    return (flags & frame_flag_sideband) != 0;
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
    uint64_t slice_seq_num{0};
    uint64_t channel_frame_seq_num{0};
    uint32_t frame_payload_size{0};
    uint64_t flags{0};
    SidebandBytes sideband_data{};
    SignatureBytes signature{};
    Hash frame_hash{};
};

// Both directions enforce every frame-local header rule and throw
// std::runtime_error on violation.
HeaderBytes serialize_frame_header(const FrameHeader &header);
FrameHeader parse_fixed_header(const uint8_t *data, std::size_t size);

std::string channel_type_name(ChannelType type);
std::string hash_hex(const Hash &hash);
Hash blake3_hash(const uint8_t *data, std::size_t size);
// BLAKE3 over the canonical image of a complete record.
Hash compute_frame_hash(const uint8_t *data, std::size_t size);
// The frame_hash the record would carry on volume `volume_seq_num`. A replay
// is equivalent to an accepted frame exactly when this matches the accepted
// frame's hash under its own volume ordinal.
Hash frame_hash_with_volume_seq(const uint8_t *data, std::size_t size,
                                uint64_t volume_seq_num);
uint32_t decoded_block_size(const FrameHeader &header);
bool valid_block_size(uint32_t block_size);

// Record-level integrity, computed once per received record. Throws only when
// the fixed header itself is invalid; the signature policy is applied
// separately by validate_frame_signature().
struct CheckedFrame {
    FrameHeader header;
    bool size_ok = false; // record length equals the decoded block size
    bool hash_ok = false; // implies size_ok
};
CheckedFrame check_frame(const uint8_t *data, std::size_t size);
std::string make_uuid_v4();

} // namespace neotape
