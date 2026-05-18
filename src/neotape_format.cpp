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
	std::size_t n = std::min(value.size(), size);
	std::memcpy(bytes.data() + offset, value.data(), n);
	if (n < size)
		bytes[offset + n] = 0;
}

std::string get_fixed_string(const uint8_t *bytes, std::size_t offset,
    std::size_t size) {
	const auto *begin = reinterpret_cast<const char *>(bytes + offset);
	const char *end = std::find(begin, begin + size, '\0');
	return std::string(begin, end);
}

// ====================== Header Common Fields =====================

HeaderBytes make_header(HeaderType type) {
	HeaderBytes bytes{};
	for (std::size_t i = 0; i < magic.size(); ++i)
		bytes[i] = static_cast<uint8_t>(magic[i]);
	bytes[8] = header_version;
	bytes[9] = static_cast<uint8_t>(type);
	return bytes;
}

void finish_crc(HeaderBytes &bytes) {
	uint32_t crc = crc32c::Crc32c(bytes.data(), fixed_header_size - 4);
	put_u32(bytes, fixed_header_size - 4, crc);
}

void check_common(const uint8_t *data, std::size_t size) {
	if (size < fixed_header_size)
		throw std::runtime_error("short fixed header");
	for (std::size_t i = 0; i < magic.size(); ++i) {
		if (data[i] != static_cast<uint8_t>(magic[i]))
			throw std::runtime_error("bad magic");
	}
	if (data[8] != header_version)
		throw std::runtime_error(std::format("unsupported header version {}", data[8]));
}

// ====================== Header Parsers ===========================

VolumeHeader parse_volume(const uint8_t *data) {
	VolumeHeader header;
	header.volume_block_size = get_u32(data, 10);
	header.archive_uuid = get_fixed_string(data, 14, 37);
	header.archive_name = get_fixed_string(data, 51, 256);
	header.volume_seq_num = get_u64(data, 307);
	header.payload_profile = static_cast<PayloadProfile>(data[315]);
	header.volume_write_at_utc = get_fixed_string(data, 316, 20);
	header.flags = get_u16(data, 336);
	return header;
}

FrameHeader parse_frame(const uint8_t *data) {
	FrameHeader header;
	header.volume_block_size = get_u32(data, 10);
	header.archive_uuid = get_fixed_string(data, 14, 37);
	header.archive_name = get_fixed_string(data, 51, 256);
	header.volume_seq_num = get_u64(data, 307);
	header.payload_profile = static_cast<PayloadProfile>(data[315]);
	header.logical_slice_seq_num = get_u64(data, 316);
	header.global_frame_seq_num = get_u64(data, 324);
	header.frame_seq_num_within_slice = get_u64(data, 332);
	header.frame_payload_size = get_u64(data, 340);
	header.frame_content_type = static_cast<FrameContentType>(data[348]);
	header.frame_payload_blake3 = get_hash(data, 349);
	header.flags = get_u16(data, 381);
	header.slice_content_size = get_u64(data, 383);
	header.slice_content_blake3 = get_hash(data, 391);
	return header;
}

ArchiveEndHeader parse_archive_end(const uint8_t *data) {
	ArchiveEndHeader header;
	header.volume_block_size = get_u32(data, 10);
	header.archive_uuid = get_fixed_string(data, 14, 37);
	header.archive_name = get_fixed_string(data, 51, 256);
	header.volume_seq_num = get_u64(data, 307);
	header.payload_profile = static_cast<PayloadProfile>(data[315]);
	header.last_logical_slice_seq_num = get_u64(data, 316);
	header.last_global_frame_seq_num = get_u64(data, 324);
	header.created_by_implementation = get_fixed_string(data, 332, 64);
	header.created_by_build_id = get_fixed_string(data, 396, 64);
	header.archive_end_at_utc = get_fixed_string(data, 460, 20);
	header.flags = get_u16(data, 480);
	return header;
}

} // namespace

// ====================== Header Serializers =======================

HeaderBytes serialize_volume_header(const VolumeHeader &header) {
	HeaderBytes bytes = make_header(HeaderType::volume);
	put_u32(bytes, 10, header.volume_block_size);
	put_fixed_string(bytes, 14, 37, header.archive_uuid);
	put_fixed_string(bytes, 51, 256, header.archive_name);
	put_u64(bytes, 307, header.volume_seq_num);
	bytes[315] = static_cast<uint8_t>(header.payload_profile);
	put_fixed_string(bytes, 316, 20, header.volume_write_at_utc);
	put_u16(bytes, 336, header.flags);
	finish_crc(bytes);
	return bytes;
}

HeaderBytes serialize_frame_header(const FrameHeader &header) {
	HeaderBytes bytes = make_header(HeaderType::frame);
	put_u32(bytes, 10, header.volume_block_size);
	put_fixed_string(bytes, 14, 37, header.archive_uuid);
	put_fixed_string(bytes, 51, 256, header.archive_name);
	put_u64(bytes, 307, header.volume_seq_num);
	bytes[315] = static_cast<uint8_t>(header.payload_profile);
	put_u64(bytes, 316, header.logical_slice_seq_num);
	put_u64(bytes, 324, header.global_frame_seq_num);
	put_u64(bytes, 332, header.frame_seq_num_within_slice);
	put_u64(bytes, 340, header.frame_payload_size);
	bytes[348] = static_cast<uint8_t>(header.frame_content_type);
	put_bytes(bytes, 349, header.frame_payload_blake3);
	put_u16(bytes, 381, header.flags);
	put_u64(bytes, 383, header.slice_content_size);
	put_bytes(bytes, 391, header.slice_content_blake3);
	finish_crc(bytes);
	return bytes;
}

HeaderBytes serialize_archive_end_header(const ArchiveEndHeader &header) {
	HeaderBytes bytes = make_header(HeaderType::archive_end);
	put_u32(bytes, 10, header.volume_block_size);
	put_fixed_string(bytes, 14, 37, header.archive_uuid);
	put_fixed_string(bytes, 51, 256, header.archive_name);
	put_u64(bytes, 307, header.volume_seq_num);
	bytes[315] = static_cast<uint8_t>(header.payload_profile);
	put_u64(bytes, 316, header.last_logical_slice_seq_num);
	put_u64(bytes, 324, header.last_global_frame_seq_num);
	put_fixed_string(bytes, 332, 64, header.created_by_implementation);
	put_fixed_string(bytes, 396, 64, header.created_by_build_id);
	put_fixed_string(bytes, 460, 20, header.archive_end_at_utc);
	put_u16(bytes, 480, header.flags);
	finish_crc(bytes);
	return bytes;
}

// ====================== Public Parse Helpers =====================

ParsedHeader parse_fixed_header(const uint8_t *data, std::size_t size) {
	check_common(data, size);

	ParsedHeader parsed;
	parsed.version = data[8];
	parsed.type = static_cast<HeaderType>(data[9]);
	parsed.stored_crc32c = get_u32(data, fixed_header_size - 4);
	parsed.computed_crc32c = crc32c::Crc32c(data, fixed_header_size - 4);
	if (parsed.stored_crc32c != parsed.computed_crc32c)
		throw std::runtime_error("header CRC32C mismatch");

	switch (parsed.type) {
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
