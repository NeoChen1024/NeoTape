#include "neotape/format.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "test_format: " << msg << "\n";
    std::exit(1);
}

void expect(bool ok, const std::string &msg) {
    if (!ok)
        fail(msg);
}

template <class Fn> void expect_throw(Fn fn, const std::string &msg) {
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
    uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(b[off + i]) << (8 * i);
    }
    return v;
}

uint64_t le64(const neotape::HeaderBytes &b, std::size_t off) {
    uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    return v;
}

neotape::FrameHeader make_content_header() {
    neotape::FrameHeader h;
    h.channel_type = neotape::ChannelType::CH_CONTENT;
    h.volume_block_size_kib = 4;
    h.archive_uuid = "00000000-0000-4000-8000-000000000123";
    h.archive_label = "unit-test";
    h.volume_seq_num = 7;
    h.global_frame_seq_num = 9;
    h.logical_slice_seq_num = 2;
    h.frame_seq_num_within_channel = 1;
    h.frame_payload_size = 123;
    h.flags = neotape::frame_flag_start | neotape::frame_flag_end;
    h.frame_hash[0] = 0xcc;
    h.frame_hash[31] = 0xdd;
    return h;
}

neotape::FrameHeader make_archive_end_header() {
    neotape::FrameHeader h;
    h.channel_type = neotape::ChannelType::ARCHIVE_END;
    h.volume_block_size_kib = 4;
    h.archive_uuid = "00000000-0000-4000-8000-000000000123";
    h.archive_label = "unit-test";
    h.volume_seq_num = 1;
    h.global_frame_seq_num = 10;
    h.logical_slice_seq_num = 0;
    h.frame_seq_num_within_channel = 1;
    h.frame_payload_size = 0;
    h.flags = neotape::frame_flag_start | neotape::frame_flag_end |
              neotape::frame_flag_clean_end;
    return h;
}

void test_channel_type_values() {
    neotape::FrameHeader h = make_content_header();

    h.channel_type = neotape::ChannelType::CH_CONTENT;
    neotape::HeaderBytes b = neotape::serialize_frame_header(h);
    expect(b[9] == 1, "bad content channel value");
    expect(neotape::parse_fixed_header(b.data(), b.size()).channel_type ==
               neotape::ChannelType::CH_CONTENT,
           "content channel should parse");

    h.channel_type = neotape::ChannelType::CH_METADATA;
    b = neotape::serialize_frame_header(h);
    expect(b[9] == 2, "bad metadata channel value");
    expect(neotape::parse_fixed_header(b.data(), b.size()).channel_type ==
               neotape::ChannelType::CH_METADATA,
           "metadata channel should parse");

    h = make_archive_end_header();
    b = neotape::serialize_frame_header(h);
    expect(b[9] == 255, "bad archive-end channel value");
    expect(neotape::parse_fixed_header(b.data(), b.size()).channel_type ==
               neotape::ChannelType::ARCHIVE_END,
           "archive-end channel should parse");
}

void test_layout_round_trip() {
    neotape::FrameHeader h = make_content_header();
    neotape::HeaderBytes b = neotape::serialize_frame_header(h);

    expect(b.size() == 512, "header size should be 512");
    expect(std::memcmp(b.data(), "NeoTape\0", 8) == 0, "bad magic");
    expect(b[8] == 1, "bad header version");
    expect(b[9] == 1, "bad channel type");
    expect(le16(b, 10) == 4, "bad volume_block_size_kib");
    expect(le64(b, 114) == 7, "bad volume_seq_num");
    expect(le64(b, 122) == 9, "bad global_frame_seq_num");
    expect(le64(b, 130) == 2, "bad logical_slice_seq_num");
    expect(le64(b, 138) == 1, "bad frame_seq_num_within_channel");
    expect(le32(b, 146) == 123, "bad frame_payload_size");
    expect(le64(b, 150) ==
               (neotape::frame_flag_start | neotape::frame_flag_end),
           "bad flags");
    expect(b[408] == 0, "unsigned signature start should be zero");
    expect(b[479] == 0, "unsigned signature end should be zero");
    expect(b[480] == 0xcc, "bad frame_hash start offset");
    expect(b[511] == 0xdd, "bad frame_hash end offset");

    neotape::FrameHeader parsed =
        neotape::parse_fixed_header(b.data(), b.size());
    expect(parsed.channel_type == neotape::ChannelType::CH_CONTENT,
           "parsed channel mismatch");
    expect(parsed.volume_block_size_kib == 4, "parsed block size mismatch");
    expect(parsed.archive_uuid == h.archive_uuid, "parsed uuid mismatch");
    expect(parsed.archive_label == h.archive_label, "parsed label mismatch");
    expect(parsed.global_frame_seq_num == 9, "parsed global seq mismatch");
    expect(parsed.frame_payload_size == 123, "parsed payload size mismatch");
    expect(parsed.signature[0] == 0 && parsed.signature[71] == 0,
           "parsed unsigned signature mismatch");
    expect(parsed.frame_hash[0] == 0xcc && parsed.frame_hash[31] == 0xdd,
           "parsed hash mismatch");
    expect(neotape::decoded_block_size(parsed) == 4096,
           "decoded block size mismatch");
}

void test_signed_frame_signature_round_trip() {
    neotape::FrameHeader h = make_content_header();
    h.flags |= neotape::frame_flag_signed;
    h.signature[0] = 0xaa;
    h.signature[71] = 0xbb;

    neotape::HeaderBytes b = neotape::serialize_frame_header(h);
    expect(b[408] == 0xaa, "signed signature start offset should be preserved");
    expect(b[479] == 0xbb, "signed signature end offset should be preserved");

    neotape::FrameHeader parsed =
        neotape::parse_fixed_header(b.data(), b.size());
    expect(parsed.signature[0] == 0xaa && parsed.signature[71] == 0xbb,
           "parsed signed signature mismatch");
}

void test_unsigned_serializer_rejects_signature() {
    neotape::FrameHeader h = make_content_header();
    h.signature[0] = 0xaa;

    expect_throw([&] { neotape::serialize_frame_header(h); },
                 "unsigned serializer should reject non-zero signature bytes");
}

void test_validation() {
    neotape::FrameHeader h = make_content_header();
    neotape::HeaderBytes b = neotape::serialize_frame_header(h);

    auto reserved = b;
    reserved[158] = 1;
    expect_throw(
        [&] { neotape::parse_fixed_header(reserved.data(), reserved.size()); },
        "reserved byte should be rejected");

    auto reserved_flag = b;
    reserved_flag[150] = static_cast<uint8_t>(reserved_flag[150] | 0x08u);
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

    b = neotape::serialize_frame_header(make_archive_end_header());
    neotape::FrameHeader parsed =
        neotape::parse_fixed_header(b.data(), b.size());
    expect(parsed.channel_type == neotape::ChannelType::ARCHIVE_END,
           "archive end should parse");

    b[157] = static_cast<uint8_t>(b[157] & 0x7fu);
    expect_throw([&] { neotape::parse_fixed_header(b.data(), b.size()); },
                 "archive end without CLEAN_END should be rejected");

    b = neotape::serialize_frame_header(make_archive_end_header());
    b[150] = static_cast<uint8_t>(b[150] & ~0x01u);
    expect_throw([&] { neotape::parse_fixed_header(b.data(), b.size()); },
                 "archive end without START should be rejected");

    b = neotape::serialize_frame_header(make_archive_end_header());
    b[150] = static_cast<uint8_t>(b[150] & ~0x02u);
    expect_throw([&] { neotape::parse_fixed_header(b.data(), b.size()); },
                 "archive end without END should be rejected");

    b = neotape::serialize_frame_header(make_archive_end_header());
    b[130] = 1;
    expect_throw([&] { neotape::parse_fixed_header(b.data(), b.size()); },
                 "archive end with logical slice should be rejected");

    b = neotape::serialize_frame_header(make_archive_end_header());
    b[138] = 2;
    expect_throw(
        [&] { neotape::parse_fixed_header(b.data(), b.size()); },
        "archive end with non-one channel sequence should be rejected");
}

void test_frame_hash_canonicalization() {
    neotape::FrameHeader h = make_content_header();
    neotape::HeaderBytes header = neotape::serialize_frame_header(h);

    std::vector<uint8_t> record(4096, 0);
    std::copy(header.begin(), header.end(), record.begin());
    record[512] = 0x42;
    record[513] = 0x43;

    neotape::Hash hash =
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

int main() {
    test_channel_type_values();
    test_layout_round_trip();
    test_signed_frame_signature_round_trip();
    test_unsigned_serializer_rejects_signature();
    test_validation();
    test_frame_hash_canonicalization();
    std::cout << "test_format: ok\n";
    return 0;
}
