// auto-generated — do not edit
#include "neotape/format_generated.hpp"
#include <crc32c/crc32c.h>

namespace neotape {

HeaderBytes serialize_volume_header(const VolumeHeader &h) {
    HeaderBytes bytes = make_header(HeaderType::volume);
    detail::put_u32(bytes, hdr_volume_block_size, h.volume_block_size);
    detail::put_fixed_string(bytes, hdr_archive_uuid, nt_uuid_size, h.archive_uuid);
    detail::put_fixed_string(bytes, hdr_archive_name, nt_name_size, h.archive_name);
    detail::put_u64(bytes, hdr_volume_seq_num, h.volume_seq_num);
    bytes[hdr_payload_profile] = static_cast<uint8_t>(h.payload_profile);
    detail::put_fixed_string(bytes, vhdr_volume_write_at_utc, nt_time_size, h.volume_write_at_utc);
    detail::put_u16(bytes, vhdr_flags, h.flags);
    finish_crc(bytes);
    return bytes;
}

HeaderBytes serialize_frame_header(const FrameHeader &h) {
    HeaderBytes bytes = make_header(HeaderType::frame);
    detail::put_u32(bytes, hdr_volume_block_size, h.volume_block_size);
    detail::put_fixed_string(bytes, hdr_archive_uuid, nt_uuid_size, h.archive_uuid);
    detail::put_fixed_string(bytes, hdr_archive_name, nt_name_size, h.archive_name);
    detail::put_u64(bytes, hdr_volume_seq_num, h.volume_seq_num);
    bytes[hdr_payload_profile] = static_cast<uint8_t>(h.payload_profile);
    detail::put_u64(bytes, fhdr_logical_slice_seq_num, h.logical_slice_seq_num);
    detail::put_u64(bytes, fhdr_global_frame_seq_num, h.global_frame_seq_num);
    detail::put_u64(bytes, fhdr_frame_seq_num_within_slice, h.frame_seq_num_within_slice);
    detail::put_u64(bytes, fhdr_frame_payload_size, h.frame_payload_size);
    bytes[fhdr_frame_content_type] = static_cast<uint8_t>(h.frame_content_type);
    detail::put_bytes(bytes, fhdr_frame_payload_blake3, h.frame_payload_blake3);
    detail::put_u16(bytes, fhdr_flags, h.flags);
    detail::put_u64(bytes, fhdr_slice_content_size, h.slice_content_size);
    detail::put_bytes(bytes, fhdr_slice_content_blake3, h.slice_content_blake3);
    finish_crc(bytes);
    return bytes;
}

HeaderBytes serialize_medium_header(const MediumHeader &h) {
    HeaderBytes bytes = make_header(HeaderType::medium);
    detail::put_u32(bytes, mhdr_medium_header_block_size, h.medium_header_block_size);
    detail::put_fixed_string(bytes, mhdr_medium_uuid, nt_uuid_size, h.medium_uuid);
    detail::put_fixed_string(bytes, mhdr_medium_label, nt_name_size, h.medium_label);
    detail::put_fixed_string(bytes, mhdr_initialized_at_utc, nt_time_size, h.initialized_at_utc);
    detail::put_u16(bytes, mhdr_medium_header_block_count, h.medium_header_block_count);
    detail::put_u16(bytes, mhdr_flags, h.flags);
    detail::put_fixed_string(bytes, mhdr_created_by_implementation, ident64_size, h.created_by_implementation);
    detail::put_fixed_string(bytes, mhdr_created_by_build_id, ident64_size, h.created_by_build_id);
    detail::put_u32(bytes, mhdr_metadata_bundle_size, h.metadata_bundle_size);
    detail::put_bytes(bytes, mhdr_metadata_bundle_blake3, h.metadata_bundle_blake3);
    finish_crc(bytes);
    return bytes;
}

HeaderBytes serialize_archive_end_header(const ArchiveEndHeader &h) {
    HeaderBytes bytes = make_header(HeaderType::archive_end);
    detail::put_u32(bytes, hdr_volume_block_size, h.volume_block_size);
    detail::put_fixed_string(bytes, hdr_archive_uuid, nt_uuid_size, h.archive_uuid);
    detail::put_fixed_string(bytes, hdr_archive_name, nt_name_size, h.archive_name);
    detail::put_u64(bytes, hdr_volume_seq_num, h.volume_seq_num);
    bytes[hdr_payload_profile] = static_cast<uint8_t>(h.payload_profile);
    detail::put_u64(bytes, ae_last_logical_slice_seq_num, h.last_logical_slice_seq_num);
    detail::put_u64(bytes, ae_last_global_frame_seq_num, h.last_global_frame_seq_num);
    detail::put_fixed_string(bytes, ae_created_by_implementation, ident64_size, h.created_by_implementation);
    detail::put_fixed_string(bytes, ae_created_by_build_id, ident64_size, h.created_by_build_id);
    detail::put_fixed_string(bytes, ae_archive_end_at_utc, nt_time_size, h.archive_end_at_utc);
    detail::put_u16(bytes, ae_flags, h.flags);
    finish_crc(bytes);
    return bytes;
}

VolumeHeader parse_volume(const uint8_t *data) {
    VolumeHeader h;
    h.volume_block_size = detail::get_u32(data, hdr_volume_block_size);
    h.archive_uuid = detail::get_fixed_string(data, hdr_archive_uuid, nt_uuid_size);
    h.archive_name = detail::get_nt_name(data, hdr_archive_name, nt_name_size);
    h.volume_seq_num = detail::get_u64(data, hdr_volume_seq_num);
    h.payload_profile = static_cast<PayloadProfile>(data[hdr_payload_profile]);
    h.volume_write_at_utc = detail::get_fixed_string(data, vhdr_volume_write_at_utc, nt_time_size);
    h.flags = detail::get_u16(data, vhdr_flags);
    return h;
}

FrameHeader parse_frame(const uint8_t *data) {
    FrameHeader h;
    h.volume_block_size = detail::get_u32(data, hdr_volume_block_size);
    h.archive_uuid = detail::get_fixed_string(data, hdr_archive_uuid, nt_uuid_size);
    h.archive_name = detail::get_nt_name(data, hdr_archive_name, nt_name_size);
    h.volume_seq_num = detail::get_u64(data, hdr_volume_seq_num);
    h.payload_profile = static_cast<PayloadProfile>(data[hdr_payload_profile]);
    h.logical_slice_seq_num = detail::get_u64(data, fhdr_logical_slice_seq_num);
    h.global_frame_seq_num = detail::get_u64(data, fhdr_global_frame_seq_num);
    h.frame_seq_num_within_slice = detail::get_u64(data, fhdr_frame_seq_num_within_slice);
    h.frame_payload_size = detail::get_u64(data, fhdr_frame_payload_size);
    h.frame_content_type = static_cast<FrameContentType>(data[fhdr_frame_content_type]);
    h.frame_payload_blake3 = detail::get_hash(data, fhdr_frame_payload_blake3);
    h.flags = detail::get_u16(data, fhdr_flags);
    h.slice_content_size = detail::get_u64(data, fhdr_slice_content_size);
    h.slice_content_blake3 = detail::get_hash(data, fhdr_slice_content_blake3);
    return h;
}

MediumHeader parse_medium(const uint8_t *data) {
    MediumHeader h;
    h.medium_header_block_size = detail::get_u32(data, mhdr_medium_header_block_size);
    h.medium_uuid = detail::get_fixed_string(data, mhdr_medium_uuid, nt_uuid_size);
    h.medium_label = detail::get_nt_name(data, mhdr_medium_label, nt_name_size);
    h.initialized_at_utc = detail::get_fixed_string(data, mhdr_initialized_at_utc, nt_time_size);
    h.medium_header_block_count = detail::get_u16(data, mhdr_medium_header_block_count);
    h.flags = detail::get_u16(data, mhdr_flags);
    h.created_by_implementation = detail::get_fixed_string(data, mhdr_created_by_implementation, ident64_size);
    h.created_by_build_id = detail::get_fixed_string(data, mhdr_created_by_build_id, ident64_size);
    h.metadata_bundle_size = detail::get_u32(data, mhdr_metadata_bundle_size);
    h.metadata_bundle_blake3 = detail::get_hash(data, mhdr_metadata_bundle_blake3);
    return h;
}

ArchiveEndHeader parse_archive_end(const uint8_t *data) {
    ArchiveEndHeader h;
    h.volume_block_size = detail::get_u32(data, hdr_volume_block_size);
    h.archive_uuid = detail::get_fixed_string(data, hdr_archive_uuid, nt_uuid_size);
    h.archive_name = detail::get_nt_name(data, hdr_archive_name, nt_name_size);
    h.volume_seq_num = detail::get_u64(data, hdr_volume_seq_num);
    h.payload_profile = static_cast<PayloadProfile>(data[hdr_payload_profile]);
    h.last_logical_slice_seq_num = detail::get_u64(data, ae_last_logical_slice_seq_num);
    h.last_global_frame_seq_num = detail::get_u64(data, ae_last_global_frame_seq_num);
    h.created_by_implementation = detail::get_fixed_string(data, ae_created_by_implementation, ident64_size);
    h.created_by_build_id = detail::get_fixed_string(data, ae_created_by_build_id, ident64_size);
    h.archive_end_at_utc = detail::get_fixed_string(data, ae_archive_end_at_utc, nt_time_size);
    h.flags = detail::get_u16(data, ae_flags);
    return h;
}

std::string header_type_name(HeaderType type) {
    switch (type) {
    case HeaderType::medium: return "medium";
    case HeaderType::volume: return "volume";
    case HeaderType::frame: return "frame";
    case HeaderType::archive_end: return "archive_end";
    }
    return "unknown";
}

std::string payload_profile_name(PayloadProfile type) {
    switch (type) {
    case PayloadProfile::raw: return "raw";
    case PayloadProfile::pax: return "pax";
    }
    return "unknown";
}

std::string frame_content_type_name(FrameContentType type) {
    switch (type) {
    case FrameContentType::slice_content: return "slice_content";
    case FrameContentType::slice_metadata: return "slice_metadata";
    }
    return "unknown";
}

} // namespace neotape