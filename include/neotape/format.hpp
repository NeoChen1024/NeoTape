#pragma once

#include "neotape/format_generated.hpp"

#include <optional>
#include <string>

namespace neotape {

struct ParsedHeader {
    HeaderType type;
    uint8_t version = 0;
    uint32_t stored_crc32c = 0;
    uint32_t computed_crc32c = 0;
    std::optional<VolumeHeader> volume;
    std::optional<FrameHeader> frame;
    std::optional<ArchiveEndHeader> archive_end;
    std::optional<MediumHeader> medium;
};

HeaderBytes serialize_volume_header(const VolumeHeader &header);
HeaderBytes serialize_medium_header(const MediumHeader &header);
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
