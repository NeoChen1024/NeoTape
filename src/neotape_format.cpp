#include "neotape/format.hpp"

#include <blake3.h>
#include <crc32c/crc32c.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <random>
#include <stdexcept>

namespace neotape {
namespace {

// ====================== Fixed Header Encoding ====================

void put_u16(HeaderBytes &bytes, std::size_t offset, uint16_t value) {
	bytes[offset] = static_cast<uint8_t>(value & 0xffu);
	bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

void put_u32(HeaderBytes &bytes, std::size_t offset, uint32_t value) {
	for (std::size_t i = 0; i < 4; ++i)
		bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);
}

void put_u64(HeaderBytes &bytes, std::size_t offset, uint64_t value) {
	for (std::size_t i = 0; i < 8; ++i)
		bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);
}

uint16_t get_u16(const uint8_t *bytes, std::size_t offset) {
	return static_cast<uint16_t>(bytes[offset]) |
	       static_cast<uint16_t>(bytes[offset + 1]) << 8;
}

uint32_t get_u32(const uint8_t *bytes, std::size_t offset) {
	uint32_t value = 0;
	for (std::size_t i = 0; i < 4; ++i)
		value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
	return value;
}

uint64_t get_u64(const uint8_t *bytes, std::size_t offset) {
	uint64_t value = 0;
	for (std::size_t i = 0; i < 8; ++i)
		value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
	return value;
}

void put_bytes(HeaderBytes &bytes, std::size_t offset, const Hash &hash) {
	std::ranges::copy(hash, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

Hash get_hash(const uint8_t *bytes, std::size_t offset) {
	Hash hash{};
	std::copy(bytes + offset, bytes + offset + hash.size(), hash.begin());
	return hash;
}

void put_fixed_string(HeaderBytes &bytes, std::size_t offset, std::size_t size,
    std::string_view value) {
	std::size_t n = std::min(value.size(), size - 1);
	std::memcpy(bytes.data() + offset, value.data(), n);
	std::memset(bytes.data() + offset + n, 0, size - n);
}

std::string get_fixed_string(const uint8_t *bytes, std::size_t offset,
    std::size_t size) {
	const auto *begin = reinterpret_cast<const char *>(bytes + offset);
	const char *end = std::find(begin, begin + size, '\0');
	return std::string(begin, end);
}

std::string get_nt_name(const uint8_t *bytes, std::size_t offset,
    std::size_t size) {
	if (bytes[offset + size - 1] != 0)
		throw std::runtime_error("nt_name field without trailing NUL");
	return get_fixed_string(bytes, offset, size);
}

// ====================== Header Common Fields =====================

HeaderBytes make_header(HeaderType type) {
	HeaderBytes bytes{};
	for (std::size_t i = 0; i < magic.size(); ++i)
		bytes[i] = static_cast<uint8_t>(magic[i]);
	bytes[com_header_version] = header_version;
	bytes[com_header_type] = static_cast<uint8_t>(type);
	return bytes;
}

void finish_crc(HeaderBytes &bytes) {
	uint32_t crc = crc32c::Crc32c(bytes.data(), fixed_header_size - 4);
	put_u32(bytes, hdr_crc32c, crc);
}

void check_common(const uint8_t *data, std::size_t size) {
	if (size < fixed_header_size)
		throw std::runtime_error("short fixed header");
	for (std::size_t i = 0; i < magic.size(); ++i) {
		if (data[i] != static_cast<uint8_t>(magic[i]))
			throw std::runtime_error("bad magic");
	}
	if (data[com_header_version] != header_version)
		throw std::runtime_error(std::format("unsupported header version {}", data[8]));
}

// ====================== Header Parsers ===========================

VolumeHeader parse_volume(const uint8_t *data) {
	VolumeHeader header;
	header.volume_block_size = get_u32(data, hdr_volume_block_size);
	header.archive_uuid = get_fixed_string(data, hdr_archive_uuid, nt_uuid_size);
	header.archive_name = get_nt_name(data, hdr_archive_name, nt_name_size);
	header.volume_seq_num = get_u64(data, hdr_volume_seq_num);
	header.payload_profile = static_cast<PayloadProfile>(data[hdr_payload_profile]);
	header.volume_write_at_utc = get_fixed_string(data, vhdr_write_at_utc, nt_time_size);
	header.flags = get_u16(data, vhdr_flags);
	return header;
}

FrameHeader parse_frame(const uint8_t *data) {
	FrameHeader header;
	header.volume_block_size = get_u32(data, hdr_volume_block_size);
	header.archive_uuid = get_fixed_string(data, hdr_archive_uuid, nt_uuid_size);
	header.archive_name = get_nt_name(data, hdr_archive_name, nt_name_size);
	header.volume_seq_num = get_u64(data, hdr_volume_seq_num);
	header.payload_profile = static_cast<PayloadProfile>(data[hdr_payload_profile]);
	header.logical_slice_seq_num = get_u64(data, fhdr_logical_slice_seq_num);
	header.global_frame_seq_num = get_u64(data, fhdr_global_frame_seq_num);
	header.frame_seq_num_within_slice = get_u64(data, fhdr_frame_seq_num_within_slice);
	header.frame_payload_size = get_u64(data, fhdr_frame_payload_size);
	header.frame_content_type = static_cast<FrameContentType>(data[fhdr_frame_content_type]);
	header.frame_payload_blake3 = get_hash(data, fhdr_frame_payload_blake3);
	header.flags = get_u16(data, fhdr_flags);
	header.slice_content_size = get_u64(data, fhdr_slice_content_size);
	header.slice_content_blake3 = get_hash(data, fhdr_slice_content_blake3);
	return header;
}

ArchiveEndHeader parse_archive_end(const uint8_t *data) {
	ArchiveEndHeader header;
	header.volume_block_size = get_u32(data, hdr_volume_block_size);
	header.archive_uuid = get_fixed_string(data, hdr_archive_uuid, nt_uuid_size);
	header.archive_name = get_nt_name(data, hdr_archive_name, nt_name_size);
	header.volume_seq_num = get_u64(data, hdr_volume_seq_num);
	header.payload_profile = static_cast<PayloadProfile>(data[hdr_payload_profile]);
	header.last_logical_slice_seq_num = get_u64(data, ae_last_logical_slice_seq_num);
	header.last_global_frame_seq_num = get_u64(data, ae_last_global_frame_seq_num);
	header.created_by_implementation = get_fixed_string(data, ae_created_by_implementation, ident64_size);
	header.created_by_build_id = get_fixed_string(data, ae_created_by_build_id, ident64_size);
	header.archive_end_at_utc = get_fixed_string(data, ae_archive_end_at_utc, nt_time_size);
	header.flags = get_u16(data, ae_flags);
	return header;
}

MediumHeader parse_medium(const uint8_t *data) {
	MediumHeader header;
	header.medium_uuid = get_fixed_string(data, mhdr_medium_uuid, nt_uuid_size);
	header.medium_label = get_fixed_string(data, mhdr_medium_label, nt_name_size);
	header.initialized_at_utc = get_fixed_string(data, mhdr_initialized_at_utc, nt_time_size);
	header.medium_header_block_size = get_u32(data, mhdr_medium_header_block_size);
	header.medium_header_block_count = get_u16(data, mhdr_medium_header_block_count);
	header.flags = get_u16(data, mhdr_flags);
	header.created_by_implementation = get_fixed_string(data, mhdr_created_by_implementation, ident64_size);
	header.created_by_build_id = get_fixed_string(data, mhdr_created_by_build_id, ident64_size);
	header.metadata_bundle_size = get_u32(data, mhdr_metadata_bundle_size);
	header.metadata_bundle_blake3 = get_hash(data, mhdr_metadata_bundle_blake3);
	return header;
}

} // namespace

// ====================== Header Serializers =======================

HeaderBytes serialize_volume_header(const VolumeHeader &header) {
	HeaderBytes bytes = make_header(HeaderType::volume);
	put_u32(bytes, hdr_volume_block_size, header.volume_block_size);
	put_fixed_string(bytes, hdr_archive_uuid, nt_uuid_size, header.archive_uuid);
	put_fixed_string(bytes, hdr_archive_name, nt_name_size, header.archive_name);
	put_u64(bytes, hdr_volume_seq_num, header.volume_seq_num);
	bytes[hdr_payload_profile] = static_cast<uint8_t>(header.payload_profile);
	put_fixed_string(bytes, vhdr_write_at_utc, nt_time_size, header.volume_write_at_utc);
	put_u16(bytes, vhdr_flags, header.flags);
	finish_crc(bytes);
	return bytes;
}

HeaderBytes serialize_medium_header(const MediumHeader &header) {
	HeaderBytes bytes = make_header(HeaderType::medium);
	put_u32(bytes, mhdr_medium_header_block_size, header.medium_header_block_size);
	put_fixed_string(bytes, mhdr_medium_uuid, nt_uuid_size, header.medium_uuid);
	put_fixed_string(bytes, mhdr_medium_label, nt_name_size, header.medium_label);
	put_fixed_string(bytes, mhdr_initialized_at_utc, nt_time_size, header.initialized_at_utc);
	put_u16(bytes, mhdr_medium_header_block_count, header.medium_header_block_count);
	put_u16(bytes, mhdr_flags, header.flags);
	put_fixed_string(bytes, mhdr_created_by_implementation, ident64_size, header.created_by_implementation);
	put_fixed_string(bytes, mhdr_created_by_build_id, ident64_size, header.created_by_build_id);
	put_u32(bytes, mhdr_metadata_bundle_size, header.metadata_bundle_size);
	put_bytes(bytes, mhdr_metadata_bundle_blake3, header.metadata_bundle_blake3);
	finish_crc(bytes);
	return bytes;
}

HeaderBytes serialize_frame_header(const FrameHeader &header) {
	HeaderBytes bytes = make_header(HeaderType::frame);
	put_u32(bytes, hdr_volume_block_size, header.volume_block_size);
	put_fixed_string(bytes, hdr_archive_uuid, nt_uuid_size, header.archive_uuid);
	put_fixed_string(bytes, hdr_archive_name, nt_name_size, header.archive_name);
	put_u64(bytes, hdr_volume_seq_num, header.volume_seq_num);
	bytes[hdr_payload_profile] = static_cast<uint8_t>(header.payload_profile);
	put_u64(bytes, fhdr_logical_slice_seq_num, header.logical_slice_seq_num);
	put_u64(bytes, fhdr_global_frame_seq_num, header.global_frame_seq_num);
	put_u64(bytes, fhdr_frame_seq_num_within_slice, header.frame_seq_num_within_slice);
	put_u64(bytes, fhdr_frame_payload_size, header.frame_payload_size);
	bytes[fhdr_frame_content_type] = static_cast<uint8_t>(header.frame_content_type);
	put_bytes(bytes, fhdr_frame_payload_blake3, header.frame_payload_blake3);
	put_u16(bytes, fhdr_flags, header.flags);
	put_u64(bytes, fhdr_slice_content_size, header.slice_content_size);
	put_bytes(bytes, fhdr_slice_content_blake3, header.slice_content_blake3);
	finish_crc(bytes);
	return bytes;
}

HeaderBytes serialize_archive_end_header(const ArchiveEndHeader &header) {
	HeaderBytes bytes = make_header(HeaderType::archive_end);
	put_u32(bytes, hdr_volume_block_size, header.volume_block_size);
	put_fixed_string(bytes, hdr_archive_uuid, nt_uuid_size, header.archive_uuid);
	put_fixed_string(bytes, hdr_archive_name, nt_name_size, header.archive_name);
	put_u64(bytes, hdr_volume_seq_num, header.volume_seq_num);
	bytes[hdr_payload_profile] = static_cast<uint8_t>(header.payload_profile);
	put_u64(bytes, ae_last_logical_slice_seq_num, header.last_logical_slice_seq_num);
	put_u64(bytes, ae_last_global_frame_seq_num, header.last_global_frame_seq_num);
	put_fixed_string(bytes, ae_created_by_implementation, ident64_size, header.created_by_implementation);
	put_fixed_string(bytes, ae_created_by_build_id, ident64_size, header.created_by_build_id);
	put_fixed_string(bytes, ae_archive_end_at_utc, nt_time_size, header.archive_end_at_utc);
	put_u16(bytes, ae_flags, header.flags);
	finish_crc(bytes);
	return bytes;
}

// ====================== Public Parse Helpers =====================

ParsedHeader parse_fixed_header(const uint8_t *data, std::size_t size) {
	check_common(data, size);

	ParsedHeader parsed;
	parsed.version = data[com_header_version];
	parsed.type = static_cast<HeaderType>(data[com_header_type]);
	parsed.stored_crc32c = get_u32(data, hdr_crc32c);
	parsed.computed_crc32c = crc32c::Crc32c(data, hdr_crc32c);
	if (parsed.stored_crc32c != parsed.computed_crc32c)
		throw std::runtime_error("header CRC32C mismatch");

	switch (parsed.type) {
	case HeaderType::medium:
		parsed.medium = parse_medium(data);
		break;
	case HeaderType::volume:
		parsed.volume = parse_volume(data);
		break;
	case HeaderType::frame:
		parsed.frame = parse_frame(data);
		break;
	case HeaderType::archive_end:
		parsed.archive_end = parse_archive_end(data);
		break;
	default:
		throw std::runtime_error("unsupported header type");
	}
	return parsed;
}

// ====================== Display & Utility Helpers ================

std::string header_type_name(HeaderType type) {
	switch (type) {
	case HeaderType::medium:
		return "medium";
	case HeaderType::volume:
		return "volume";
	case HeaderType::frame:
		return "frame";
	case HeaderType::archive_end:
		return "archive_end";
	}
	return "unknown";
}

std::string payload_profile_name(PayloadProfile profile) {
	switch (profile) {
	case PayloadProfile::raw:
		return "raw";
	case PayloadProfile::pax:
		return "pax";
	}
	return "unknown";
}

std::string frame_content_type_name(FrameContentType type) {
	switch (type) {
	case FrameContentType::slice_content:
		return "SLICE_CONTENT";
	case FrameContentType::slice_metadata:
		return "SLICE_METADATA";
	}
	return "unknown";
}

std::string hash_hex(const Hash &hash) {
	std::string hex;
	for (uint8_t byte : hash)
		hex += std::format("{:02x}", static_cast<unsigned>(byte));
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

std::string utc_timestamp_now() {
	std::time_t now = std::chrono::system_clock::to_time_t(
	    std::chrono::system_clock::now());
	std::tm utc{};
	if (gmtime_r(&now, &utc) == nullptr)
		throw std::runtime_error("gmtime_r failed");

	std::array<char, 20> buffer{};
	if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &utc) !=
	    19)
		throw std::runtime_error("strftime failed");
	return std::string(buffer.data(), 19);
}

std::string make_uuid_v4() {
	std::array<uint8_t, 16> bytes{};
	std::random_device rd;
	for (uint8_t &byte : bytes)
		byte = static_cast<uint8_t>(rd());
	bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
	bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
	return std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
			   "{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
	    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
	    bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
	    bytes[14], bytes[15]);
}

bool valid_block_size(uint32_t block_size) {
	return block_size >= min_block_size && block_size <= max_block_size;
}

} // namespace neotape
