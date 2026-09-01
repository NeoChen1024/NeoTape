#include "neotape/format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using std::string;

void fail(const string &msg) {
    FAIL(msg);
}

void expect(bool ok, const string &msg) {
    if (!ok) {
        fail(msg);
    }
}

template <class Fn> void expect_throw(Fn fn, const string &msg) {
    try {
        fn();
    } catch (const std::exception &) {
        return;
    }
    fail(msg);
}

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

void test_channel_type_values() {
    neotape::FrameHeader header = make_content_header();

    header.channel_type = neotape::ChannelType::CH_CONTENT;
    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);
    expect(bytes[9] == 1, "bad content channel value");
    expect(
        neotape::parse_fixed_header(bytes.data(), bytes.size()).channel_type ==
            neotape::ChannelType::CH_CONTENT,
        "content channel should parse");

    header.channel_type = neotape::ChannelType::CH_METADATA;
    bytes = neotape::serialize_frame_header(header);
    expect(bytes[9] == 2, "bad metadata channel value");
    expect(
        neotape::parse_fixed_header(bytes.data(), bytes.size()).channel_type ==
            neotape::ChannelType::CH_METADATA,
        "metadata channel should parse");

    header.channel_type = neotape::ChannelType::CH_FEC;
    header.flags = neotape::frame_flag_sideband;
    header.sideband_data[0] = 1;
    bytes = neotape::serialize_frame_header(header);
    expect(bytes[9] == 3, "bad FEC channel value");
    expect(bytes[280] == 1, "bad sideband start offset");
    neotape::FrameHeader const fec_parsed =
        neotape::parse_fixed_header(bytes.data(), bytes.size());
    expect(fec_parsed.channel_type == neotape::ChannelType::CH_FEC,
           "FEC channel should parse");
    expect(fec_parsed.sideband_data[0] == 1, "FEC sideband should round trip");

    header = make_archive_end_header();
    bytes = neotape::serialize_frame_header(header);
    expect(bytes[9] == 255, "bad archive-end channel value");
    expect(
        neotape::parse_fixed_header(bytes.data(), bytes.size()).channel_type ==
            neotape::ChannelType::ARCHIVE_END,
        "archive-end channel should parse");
}

void test_layout_round_trip() {
    neotape::FrameHeader const header = make_content_header();
    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);

    expect(bytes.size() == 512, "header size should be 512");
    expect(std::memcmp(bytes.data(), "NeoTape\0", 8) == 0, "bad magic");
    expect(bytes[8] == 1, "bad header version");
    expect(bytes[9] == 1, "bad channel type");
    expect(le16(bytes, 10) == 4, "bad volume_block_size_kib");
    expect(le64(bytes, 114) == 7, "bad volume_seq_num");
    expect(le64(bytes, 122) == 9, "bad global_frame_seq_num");
    expect(le64(bytes, 130) == 2, "bad slice_seq_num");
    expect(le64(bytes, 138) == 0, "bad channel_frame_seq_num");
    expect(le32(bytes, 146) == 123, "bad frame_payload_size");
    expect(le64(bytes, 150) == neotape::frame_flag_end, "bad flags");
    expect(bytes[408] == 0, "unsigned signature start should be zero");
    expect(bytes[479] == 0, "unsigned signature end should be zero");
    expect(bytes[480] == 0xcc, "bad frame_hash start offset");
    expect(bytes[511] == 0xdd, "bad frame_hash end offset");

    neotape::FrameHeader parsed =
        neotape::parse_fixed_header(bytes.data(), bytes.size());
    expect(parsed.channel_type == neotape::ChannelType::CH_CONTENT,
           "parsed channel mismatch");
    expect(parsed.volume_block_size_kib == 4, "parsed block size mismatch");
    expect(parsed.archive_uuid == header.archive_uuid, "parsed uuid mismatch");
    expect(parsed.archive_label == header.archive_label,
           "parsed label mismatch");
    expect(parsed.global_frame_seq_num == 9, "parsed global seq mismatch");
    expect(parsed.slice_seq_num == 2, "parsed slice seq mismatch");
    expect(parsed.channel_frame_seq_num == 0,
           "parsed channel frame seq mismatch");
    expect(parsed.frame_payload_size == 123, "parsed payload size mismatch");
    expect(parsed.signature[0] == 0 && parsed.signature[71] == 0,
           "parsed unsigned signature mismatch");
    expect(parsed.frame_hash[0] == 0xcc && parsed.frame_hash[31] == 0xdd,
           "parsed hash mismatch");
    expect(neotape::decoded_block_size(parsed) == 4096,
           "decoded block size mismatch");
}

void test_signed_frame_signature_round_trip() {
    neotape::FrameHeader header = make_content_header();
    header.flags |= neotape::frame_flag_signed;
    header.signature[0] = 0xaa;
    header.signature[71] = 0xbb;

    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);
    expect(bytes[408] == 0xaa, "signed signature start offset should persist");
    expect(bytes[479] == 0xbb, "signed signature end offset should persist");

    neotape::FrameHeader parsed =
        neotape::parse_fixed_header(bytes.data(), bytes.size());
    expect(parsed.signature[0] == 0xaa && parsed.signature[71] == 0xbb,
           "parsed signed signature mismatch");
}

void test_unsigned_serializer_rejects_signature() {
    neotape::FrameHeader header = make_content_header();
    header.signature[0] = 0xaa;

    expect_throw([&] { neotape::serialize_frame_header(header); },
                 "unsigned serializer should reject signature bytes");
}

void test_validation() {
    neotape::FrameHeader const header = make_content_header();
    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);

    auto reserved = bytes;
    reserved[158] = 1;
    expect_throw(
        [&] { neotape::parse_fixed_header(reserved.data(), reserved.size()); },
        "reserved byte should be rejected");

    auto reserved_flag = bytes;
    reserved_flag[150] = static_cast<uint8_t>(reserved_flag[150] | 0x04u);
    expect_throw(
        [&] {
            neotape::parse_fixed_header(reserved_flag.data(),
                                        reserved_flag.size());
        },
        "reserved flag bit should be rejected");

    auto content_clean_end =
        neotape::serialize_frame_header(make_content_header());
    content_clean_end[157] =
        static_cast<uint8_t>(content_clean_end[157] | 0x80u);
    expect_throw(
        [&] {
            neotape::parse_fixed_header(content_clean_end.data(),
                                        content_clean_end.size());
        },
        "CLEAN_END on content frame should be rejected");

    bytes = neotape::serialize_frame_header(make_archive_end_header());
    neotape::FrameHeader const parsed =
        neotape::parse_fixed_header(bytes.data(), bytes.size());
    expect(parsed.channel_type == neotape::ChannelType::ARCHIVE_END,
           "archive end should parse");

    bytes[157] = static_cast<uint8_t>(bytes[157] & 0x7fu);
    expect_throw(
        [&] { neotape::parse_fixed_header(bytes.data(), bytes.size()); },
        "archive end without CLEAN_END should be rejected");

    bytes = neotape::serialize_frame_header(make_archive_end_header());
    bytes[150] = static_cast<uint8_t>(bytes[150] & ~0x01u);
    expect_throw(
        [&] { neotape::parse_fixed_header(bytes.data(), bytes.size()); },
        "archive end without END should be rejected");

    bytes = neotape::serialize_frame_header(make_archive_end_header());
    bytes[130] = 1;
    expect_throw(
        [&] { neotape::parse_fixed_header(bytes.data(), bytes.size()); },
        "archive end with non-zero slice_seq_num should be rejected");

    bytes = neotape::serialize_frame_header(make_archive_end_header());
    bytes[138] = 1;
    expect_throw(
        [&] { neotape::parse_fixed_header(bytes.data(), bytes.size()); },
        "archive end with non-zero channel_frame_seq_num should be rejected");
}

void test_frame_hash_canonicalization() {
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
    expect(hash == neotape::blake3_hash(canonical.data(), canonical.size()),
           "canonical hash mismatch");

    std::vector<uint8_t> changed_sig_and_hash = record;
    changed_sig_and_hash[408] ^= 0xff;
    changed_sig_and_hash[480] ^= 0xff;
    expect(hash == neotape::compute_frame_hash(changed_sig_and_hash.data(),
                                               changed_sig_and_hash.size()),
           "signature/hash bytes must be ignored by canonical hash");

    std::vector<uint8_t> changed_payload = record;
    changed_payload[512] ^= 0xff;
    expect(hash != neotape::compute_frame_hash(changed_payload.data(),
                                               changed_payload.size()),
           "payload changes must affect canonical hash");
}

} // namespace

TEST_CASE("NeoTape frame format", "[unit][format]") {
    test_channel_type_values();
    test_layout_round_trip();
    test_signed_frame_signature_round_trip();
    test_unsigned_serializer_rejects_signature();
    test_validation();
    test_frame_hash_canonicalization();
}
