#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace neotape {

inline constexpr std::size_t fixed_header_size = 1024;
inline constexpr std::array<char, 8> magic = {'N', 'e', 'o', 'T', 'a', 'p', 'e', '\0'};
inline constexpr uint8_t header_version = 1;
inline constexpr uint32_t min_block_size = 4096;
inline constexpr uint32_t max_block_size = 16 * 1024 * 1024;

// Common prefix offsets (all header types)
inline constexpr std::size_t com_header_version = 8;
inline constexpr std::size_t com_header_type    = 9;

// Shared identity block offsets (volume, frame, archive-end)
inline constexpr std::size_t hdr_volume_block_size = 10;
inline constexpr std::size_t hdr_archive_uuid      = 14;
inline constexpr std::size_t hdr_archive_name      = 51;
inline constexpr std::size_t hdr_volume_seq_num    = 307;
inline constexpr std::size_t hdr_payload_profile   = 315;
inline constexpr std::size_t hdr_crc32c            = 1020;

// Volume header specific
inline constexpr std::size_t vhdr_write_at_utc     = 316;
inline constexpr std::size_t vhdr_flags            = 336;

// Frame header specific
inline constexpr std::size_t fhdr_logical_slice_seq_num     = 316;
inline constexpr std::size_t fhdr_global_frame_seq_num       = 324;
inline constexpr std::size_t fhdr_frame_seq_num_within_slice = 332;
inline constexpr std::size_t fhdr_frame_payload_size         = 340;
inline constexpr std::size_t fhdr_frame_content_type         = 348;
inline constexpr std::size_t fhdr_frame_payload_blake3       = 349;
inline constexpr std::size_t fhdr_flags                      = 381;
inline constexpr std::size_t fhdr_slice_content_size         = 383;
inline constexpr std::size_t fhdr_slice_content_blake3       = 391;

// Archive end header specific
inline constexpr std::size_t ae_last_logical_slice_seq_num = 316;
inline constexpr std::size_t ae_last_global_frame_seq_num  = 324;
inline constexpr std::size_t ae_created_by_implementation  = 332;
inline constexpr std::size_t ae_created_by_build_id        = 396;
inline constexpr std::size_t ae_archive_end_at_utc         = 460;
inline constexpr std::size_t ae_flags                      = 480;

// Fixed string field sizes
inline constexpr std::size_t nt_uuid_size   = 37;
inline constexpr std::size_t nt_name_size   = 256;
inline constexpr std::size_t nt_time_size   = 20;
inline constexpr std::size_t ident64_size   = 64;

enum class HeaderType : uint8_t {
	medium = 1,
	volume = 2,
	frame = 3,
	archive_end = 4,
};

enum class PayloadProfile : uint8_t {
	raw = 1,
};

enum class FrameContentType : uint8_t {
	slice_content = 1,
	slice_metadata = 2,
};

inline constexpr uint16_t frame_flag_start = 1u << 0;
inline constexpr uint16_t frame_flag_end = 1u << 1;
inline constexpr uint16_t archive_end_flag_clean_end = 1u << 0;

using HeaderBytes = std::array<uint8_t, fixed_header_size>;
using Hash = std::array<uint8_t, 32>;

struct VolumeHeader {
	uint32_t volume_block_size = 0;
	std::string archive_uuid;
	std::string archive_name;
	uint64_t volume_seq_num = 0;
	PayloadProfile payload_profile = PayloadProfile::raw;
	std::string volume_write_at_utc;
	uint16_t flags = 0;
};

struct FrameHeader {
	uint32_t volume_block_size = 0;
	std::string archive_uuid;
	std::string archive_name;
	uint64_t volume_seq_num = 0;
	PayloadProfile payload_profile = PayloadProfile::raw;
	uint64_t logical_slice_seq_num = 0;
	uint64_t global_frame_seq_num = 0;
	uint64_t frame_seq_num_within_slice = 0;
	uint64_t frame_payload_size = 0;
	FrameContentType frame_content_type = FrameContentType::slice_content;
	Hash frame_payload_blake3{};
	uint16_t flags = 0;
	uint64_t slice_content_size = 0;
	Hash slice_content_blake3{};
};

struct ArchiveEndHeader {
	uint32_t volume_block_size = 0;
	std::string archive_uuid;
	std::string archive_name;
	uint64_t volume_seq_num = 0;
	PayloadProfile payload_profile = PayloadProfile::raw;
	uint64_t last_logical_slice_seq_num = 0;
	uint64_t last_global_frame_seq_num = 0;
	std::string created_by_implementation;
	std::string created_by_build_id;
	std::string archive_end_at_utc;
	uint16_t flags = archive_end_flag_clean_end;
};

struct ParsedHeader {
	HeaderType type;
	uint8_t version = 0;
	uint32_t stored_crc32c = 0;
	uint32_t computed_crc32c = 0;
	std::optional<VolumeHeader> volume;
	std::optional<FrameHeader> frame;
	std::optional<ArchiveEndHeader> archive_end;
};

HeaderBytes serialize_volume_header(const VolumeHeader &header);
HeaderBytes serialize_frame_header(const FrameHeader &header);
HeaderBytes serialize_archive_end_header(const ArchiveEndHeader &header);
ParsedHeader parse_fixed_header(const uint8_t *data, std::size_t size);

std::string header_type_name(HeaderType type);
std::string payload_profile_name(PayloadProfile profile);
std::string frame_content_type_name(FrameContentType type);
std::string hash_hex(const Hash &hash);
Hash blake3_hash(const uint8_t *data, std::size_t size);
std::string utc_timestamp_now();
std::string make_uuid_v4();
bool valid_block_size(uint32_t block_size);

} // namespace neotape
