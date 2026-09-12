#include "neotape/format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using std::string;

uint16_t le16(const neotape::HeaderBytes &b, std::size_t off) {
    return static_cast<uint16_t>(b[off]) | static_cast<uint16_t>(b[off + 1])
                                               << 8;
}

uint32_t le32(const neotape::HeaderBytes &b, std::size_t off) {
    uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(b[off + i]) << (8 * i);
    }
    return value;
}

uint64_t le64(const neotape::HeaderBytes &b, std::size_t off) {
    uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    }
    return value;
}

neotape::FrameHeader make_content_header() {
    neotape::FrameHeader header;
    header.channel_type = neotape::ChannelType::CH_CONTENT;
    header.volume_block_size_kib = 4;
    header.archive_uuid = "00000000-0000-4000-8000-000000000123";
    header.archive_label = "unit-test";
    header.volume_seq_num = 7;
    header.global_frame_seq_num = 9;
    header.slice_seq_num = 2;
    header.channel_frame_seq_num = 0;
    header.frame_payload_size = 123;
    header.flags = neotape::frame_flag_end;
    header.frame_hash[0] = 0xcc;
    header.frame_hash[31] = 0xdd;
    return header;
}

neotape::FrameHeader make_archive_end_header() {
    neotape::FrameHeader header;
    header.channel_type = neotape::ChannelType::ARCHIVE_END;
    header.volume_block_size_kib = 4;
    header.archive_uuid = "00000000-0000-4000-8000-000000000123";
    header.archive_label = "unit-test";
    header.volume_seq_num = 1;
    header.global_frame_seq_num = 10;
    header.slice_seq_num = 0;
    header.channel_frame_seq_num = 0;
    header.frame_payload_size = 0;
    header.flags = neotape::frame_flag_end | neotape::frame_flag_clean_end;
    return header;
}

TEST_CASE("format: channel type values", "[unit][format]") {
    neotape::FrameHeader header = make_content_header();

    header.channel_type = neotape::ChannelType::CH_CONTENT;
    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);
    REQUIRE(bytes[9] == 1);
    REQUIRE(
        neotape::parse_fixed_header(bytes.data(), bytes.size()).channel_type ==
        neotape::ChannelType::CH_CONTENT);

    header.channel_type = neotape::ChannelType::CH_METADATA;
    bytes = neotape::serialize_frame_header(header);
    REQUIRE(bytes[9] == 2);
    REQUIRE(
        neotape::parse_fixed_header(bytes.data(), bytes.size()).channel_type ==
        neotape::ChannelType::CH_METADATA);

    header.channel_type = neotape::ChannelType::CH_FEC;
    header.flags = neotape::frame_flag_sideband;
    header.sideband_data[0] = 1;
    bytes = neotape::serialize_frame_header(header);
    REQUIRE(bytes[9] == 3);
    REQUIRE(bytes[280] == 1);
    neotape::FrameHeader const fec_parsed =
        neotape::parse_fixed_header(bytes.data(), bytes.size());
    REQUIRE(fec_parsed.channel_type == neotape::ChannelType::CH_FEC);
    REQUIRE(fec_parsed.sideband_data[0] == 1);

    header = make_archive_end_header();
    bytes = neotape::serialize_frame_header(header);
    REQUIRE(bytes[9] == 255);
    REQUIRE(
        neotape::parse_fixed_header(bytes.data(), bytes.size()).channel_type ==
        neotape::ChannelType::ARCHIVE_END);
}

TEST_CASE("format: layout round trip", "[unit][format]") {
    neotape::FrameHeader const header = make_content_header();
    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);

    REQUIRE(bytes.size() == 512);
    REQUIRE(std::memcmp(bytes.data(), "NeoTape\0", 8) == 0);
    REQUIRE(bytes[8] == 1);
    REQUIRE(bytes[9] == 1);
    REQUIRE(le16(bytes, 10) == 4);
    REQUIRE(le64(bytes, 114) == 7);
    REQUIRE(le64(bytes, 122) == 9);
    REQUIRE(le64(bytes, 130) == 2);
    REQUIRE(le64(bytes, 138) == 0);
    REQUIRE(le32(bytes, 146) == 123);
    REQUIRE(le64(bytes, 150) == neotape::frame_flag_end);
    REQUIRE(bytes[408] == 0);
    REQUIRE(bytes[479] == 0);
    REQUIRE(bytes[480] == 0xcc);
    REQUIRE(bytes[511] == 0xdd);

    neotape::FrameHeader parsed =
        neotape::parse_fixed_header(bytes.data(), bytes.size());
    REQUIRE(parsed.channel_type == neotape::ChannelType::CH_CONTENT);
    REQUIRE(parsed.volume_block_size_kib == 4);
    REQUIRE(parsed.archive_uuid == header.archive_uuid);
    REQUIRE(parsed.archive_label == header.archive_label);
    REQUIRE(parsed.global_frame_seq_num == 9);
    REQUIRE(parsed.slice_seq_num == 2);
    REQUIRE(parsed.channel_frame_seq_num == 0);
    REQUIRE(parsed.frame_payload_size == 123);
    REQUIRE((parsed.signature[0] == 0 && parsed.signature[71] == 0));
    REQUIRE((parsed.frame_hash[0] == 0xcc && parsed.frame_hash[31] == 0xdd));
    REQUIRE(neotape::decoded_block_size(parsed) == 4096);
}

TEST_CASE("format: signed frame signature round trip", "[unit][format]") {
    neotape::FrameHeader header = make_content_header();
    header.flags |= neotape::frame_flag_signed;
    header.signature[0] = 0xaa;
    header.signature[71] = 0xbb;

    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);
    REQUIRE(bytes[408] == 0xaa);
    REQUIRE(bytes[479] == 0xbb);

    neotape::FrameHeader parsed =
        neotape::parse_fixed_header(bytes.data(), bytes.size());
    REQUIRE((parsed.signature[0] == 0xaa && parsed.signature[71] == 0xbb));
}

TEST_CASE("format: unsigned serializer rejects signature", "[unit][format]") {
    neotape::FrameHeader header = make_content_header();
    header.signature[0] = 0xaa;

    REQUIRE_THROWS_AS(neotape::serialize_frame_header(header), std::exception);
}

TEST_CASE("format: validation", "[unit][format]") {
    neotape::FrameHeader const header = make_content_header();
    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);

    auto reserved = bytes;
    reserved[158] = 1;
    REQUIRE_THROWS_AS(
        neotape::parse_fixed_header(reserved.data(), reserved.size()),
        std::exception);

    auto reserved_flag = bytes;
    reserved_flag[150] = static_cast<uint8_t>(reserved_flag[150] | 0x04u);
    REQUIRE_THROWS_AS(
        neotape::parse_fixed_header(reserved_flag.data(), reserved_flag.size()),
        std::exception);

    auto content_clean_end =
        neotape::serialize_frame_header(make_content_header());
    content_clean_end[157] =
        static_cast<uint8_t>(content_clean_end[157] | 0x80u);
    REQUIRE_THROWS_AS(neotape::parse_fixed_header(content_clean_end.data(),
                                                  content_clean_end.size()),
                      std::exception);

    bytes = neotape::serialize_frame_header(make_archive_end_header());
    neotape::FrameHeader const parsed =
        neotape::parse_fixed_header(bytes.data(), bytes.size());
    REQUIRE(parsed.channel_type == neotape::ChannelType::ARCHIVE_END);

    bytes[157] = static_cast<uint8_t>(bytes[157] & 0x7fu);
    REQUIRE_THROWS_AS(neotape::parse_fixed_header(bytes.data(), bytes.size()),
                      std::exception);

    bytes = neotape::serialize_frame_header(make_archive_end_header());
    bytes[150] = static_cast<uint8_t>(bytes[150] & ~0x01u);
    REQUIRE_THROWS_AS(neotape::parse_fixed_header(bytes.data(), bytes.size()),
                      std::exception);

    bytes = neotape::serialize_frame_header(make_archive_end_header());
    bytes[130] = 1;
    REQUIRE_THROWS_AS(neotape::parse_fixed_header(bytes.data(), bytes.size()),
                      std::exception);

    bytes = neotape::serialize_frame_header(make_archive_end_header());
    bytes[138] = 1;
    REQUIRE_THROWS_AS(neotape::parse_fixed_header(bytes.data(), bytes.size()),
                      std::exception);
}

TEST_CASE("format: frame hash canonicalization", "[unit][format]") {
    neotape::FrameHeader const header = make_content_header();
    neotape::HeaderBytes header_bytes = neotape::serialize_frame_header(header);

    std::vector<uint8_t> record(4096, 0);
    std::copy(header_bytes.begin(), header_bytes.end(), record.begin());
    record[512] = 0x42;
    record[513] = 0x43;

    neotape::Hash const hash =
        neotape::compute_frame_hash(record.data(), record.size());

    std::vector<uint8_t> canonical = record;
    std::fill(canonical.begin() + 408, canonical.begin() + 480, 0);
    std::fill(canonical.begin() + 480, canonical.begin() + 512, 0);
    REQUIRE(hash == neotape::blake3_hash(canonical.data(), canonical.size()));

    std::vector<uint8_t> changed_sig_and_hash = record;
    changed_sig_and_hash[408] ^= 0xff;
    changed_sig_and_hash[480] ^= 0xff;
    REQUIRE(hash == neotape::compute_frame_hash(changed_sig_and_hash.data(),
                                                changed_sig_and_hash.size()));

    std::vector<uint8_t> changed_payload = record;
    changed_payload[512] ^= 0xff;
    REQUIRE(hash != neotape::compute_frame_hash(changed_payload.data(),
                                                changed_payload.size()));
}

} // namespace
