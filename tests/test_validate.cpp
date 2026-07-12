#include "neotape/fec.hpp"
#include "neotape/format.hpp"
#include "neotape/validate.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using neotape::ChannelType;
using neotape::FrameHeader;
using neotape::FrameValidator;
using neotape::RestoreFrameValidation;
using neotape::RestoreFrameValidationStatus;
using std::string;
using std::string_view;
using std::vector;

[[noreturn]] void fail(const string &msg) {
    std::cerr << "test_validate: " << msg << "\n";
    std::exit(1);
}

void expect(bool ok, const string &msg) {
    if (!ok) {
        fail(msg);
    }
}

void expect_contains(string_view haystack, string_view needle,
                     const string &context) {
    if (haystack.find(needle) == string_view::npos) {
        fail(context + ": expected to find \"" + string(needle) + "\" in \"" +
             string(haystack) + "\"");
    }
}

FrameHeader make_header(ChannelType type, uint64_t global_seq_num,
                        uint64_t slice_seq_num, uint64_t channel_frame_seq_num,
                        uint32_t payload_size, uint64_t flags) {
    FrameHeader header;
    header.channel_type = type;
    header.volume_block_size_kib = 4;
    header.archive_uuid = "00000000-0000-4000-8000-000000000123";
    header.archive_label = "validate-test";
    header.volume_seq_num = 1;
    header.global_frame_seq_num = global_seq_num;
    header.slice_seq_num = slice_seq_num;
    header.channel_frame_seq_num = channel_frame_seq_num;
    header.frame_payload_size = payload_size;
    header.flags = flags;
    return header;
}

FrameHeader make_content_header(uint64_t global_seq_num, uint64_t slice_seq_num,
                                uint64_t channel_frame_seq_num,
                                uint32_t payload_size, uint64_t flags) {
    return make_header(ChannelType::CH_CONTENT, global_seq_num, slice_seq_num,
                       channel_frame_seq_num, payload_size, flags);
}

FrameHeader make_metadata_header(uint64_t global_seq_num,
                                 uint64_t slice_seq_num,
                                 uint64_t channel_frame_seq_num,
                                 uint32_t payload_size, uint64_t flags) {
    return make_header(ChannelType::CH_METADATA, global_seq_num, slice_seq_num,
                       channel_frame_seq_num, payload_size, flags);
}

FrameHeader make_archive_end_header(uint64_t global_seq_num) {
    return make_header(ChannelType::ARCHIVE_END, global_seq_num, 0, 0, 0,
                       neotape::frame_flag_end | neotape::frame_flag_clean_end);
}

FrameHeader make_fec_header(uint64_t global_seq_num,
                            uint64_t channel_frame_seq_num,
                            const neotape::FecDescriptor &descriptor,
                            bool end) {
    FrameHeader header = make_header(
        ChannelType::CH_FEC, global_seq_num, 0, channel_frame_seq_num,
        4096 - neotape::fixed_header_size,
        neotape::frame_flag_sideband | (end ? neotape::frame_flag_end : 0));
    header.sideband_data = neotape::serialize_fec_descriptor(descriptor);
    return header;
}

vector<std::byte> build_record(FrameHeader header,
                               const vector<std::byte> &payload = {}) {
    expect(payload.size() == header.frame_payload_size,
           "payload size must match frame header");

    vector<std::byte> record(neotape::decoded_block_size(header), std::byte{0});
    neotape::HeaderBytes bytes = neotape::serialize_frame_header(header);
    std::transform(bytes.begin(), bytes.end(), record.begin(),
                   [](uint8_t value) { return static_cast<std::byte>(value); });
    std::copy(payload.begin(), payload.end(),
              record.begin() +
                  static_cast<std::ptrdiff_t>(neotape::fixed_header_size));

    header.frame_hash = neotape::compute_frame_hash(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
    bytes = neotape::serialize_frame_header(header);
    std::transform(bytes.begin(), bytes.end(), record.begin(),
                   [](uint8_t value) { return static_cast<std::byte>(value); });
    return record;
}

FrameHeader parse_header(const vector<std::byte> &record) {
    return neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
}

void expect_validation_error(const std::optional<string> &err,
                             string_view needle, const string &context) {
    if (!err.has_value()) {
        fail(context + ": expected validation error");
    }
    expect_contains(*err, needle, context);
}

void expect_restore_status(const RestoreFrameValidation &result,
                           RestoreFrameValidationStatus expected,
                           const string &context) {
    if (result.status != expected) {
        fail(context + ": unexpected restore validation status");
    }
}

void test_restore_mode_metadata_hash_warning() {
    FrameValidator validator;

    vector<std::byte> const metadata_payload = {std::byte{'m'}, std::byte{'e'},
                                                std::byte{'t'}, std::byte{'a'}};
    auto metadata_record =
        build_record(make_metadata_header(0, 0, 0, metadata_payload.size(),
                                          neotape::frame_flag_end),
                     metadata_payload);
    metadata_record[neotape::fixed_header_size] ^= std::byte{0x01};

    FrameHeader const metadata_header = parse_header(metadata_record);
    RestoreFrameValidation const metadata_result =
        validator.validate_restore_frame(
            metadata_header,
            reinterpret_cast<const uint8_t *>(metadata_record.data()),
            metadata_record.size());
    expect_restore_status(metadata_result,
                          RestoreFrameValidationStatus::warning,
                          "metadata hash mismatch should be warning-only");
    expect_contains(metadata_result.message, "metadata frame hash mismatch",
                    "metadata warning text");
    expect(validator.expected_global_frame_seq == 1,
           "metadata warning should still advance global sequence state");

    vector<std::byte> const content_payload = {std::byte{'o'}, std::byte{'k'}};
    auto content_record =
        build_record(make_content_header(1, 0, 0, content_payload.size(),
                                         neotape::frame_flag_end),
                     content_payload);
    FrameHeader const content_header = parse_header(content_record);
    RestoreFrameValidation const content_result =
        validator.validate_restore_frame(
            content_header,
            reinterpret_cast<const uint8_t *>(content_record.data()),
            content_record.size());
    expect_restore_status(content_result, RestoreFrameValidationStatus::ok,
                          "content after metadata warning should remain valid");

    auto archive_end_record = build_record(make_archive_end_header(2));
    FrameHeader const archive_end_header = parse_header(archive_end_record);
    RestoreFrameValidation const archive_end_result =
        validator.validate_restore_frame(
            archive_end_header,
            reinterpret_cast<const uint8_t *>(archive_end_record.data()),
            archive_end_record.size());
    expect_restore_status(archive_end_result, RestoreFrameValidationStatus::ok,
                          "archive_end after metadata warning should validate");
    expect(validator.saw_archive_end,
           "restore validation should mark archive_end as seen");
}

void test_restore_mode_metadata_structural_failure_is_fatal() {
    FrameValidator validator;

    vector<std::byte> const metadata_payload = {std::byte{'m'}};
    auto first_record =
        build_record(make_metadata_header(0, 0, 0, metadata_payload.size(),
                                          neotape::frame_flag_end),
                     metadata_payload);
    auto first_result = validator.validate_restore_frame(
        parse_header(first_record),
        reinterpret_cast<const uint8_t *>(first_record.data()),
        first_record.size());
    expect_restore_status(first_result, RestoreFrameValidationStatus::ok,
                          "first metadata frame should validate");

    FrameHeader second_header = make_metadata_header(
        1, 1, 0, metadata_payload.size(), neotape::frame_flag_end);
    second_header.archive_uuid = "00000000-0000-4000-8000-000000000999";
    auto second_record = build_record(second_header, metadata_payload);
    RestoreFrameValidation const second_result =
        validator.validate_restore_frame(
            parse_header(second_record),
            reinterpret_cast<const uint8_t *>(second_record.data()),
            second_record.size());
    expect_restore_status(second_result, RestoreFrameValidationStatus::fatal,
                          "metadata structural mismatch should stay fatal");
    expect_contains(second_result.message, "archive_uuid mismatch",
                    "metadata structural mismatch text");
}

void test_validator_rejects_first_frame_with_nonzero_channel_frame_seq() {
    FrameValidator validator;

    vector<std::byte> const payload = {std::byte{'x'}};
    auto record = build_record(
        make_content_header(0, 0, 1, payload.size(), neotape::frame_flag_end),
        payload);
    expect_validation_error(
        validator.validate(parse_header(record),
                           reinterpret_cast<const uint8_t *>(record.data()),
                           record.size()),
        "channel_frame_seq_num 1 != expected 0",
        "first frame with non-zero channel sequence should be rejected");
}

void test_validator_rejects_new_slice_without_channel_frame_seq_reset() {
    FrameValidator validator;

    vector<std::byte> const payload = {std::byte{'a'}};
    auto first_record = build_record(
        make_content_header(0, 0, 0, payload.size(), neotape::frame_flag_end),
        payload);
    expect(
        !validator
             .validate(parse_header(first_record),
                       reinterpret_cast<const uint8_t *>(first_record.data()),
                       first_record.size())
             .has_value(),
        "first slice should validate");

    auto second_record = build_record(
        make_content_header(1, 1, 1, payload.size(), neotape::frame_flag_end),
        payload);
    expect_validation_error(
        validator.validate(
            parse_header(second_record),
            reinterpret_cast<const uint8_t *>(second_record.data()),
            second_record.size()),
        "channel_frame_seq_num 1 != expected 0",
        "new slice without channel sequence reset should be rejected");
}

void test_validator_rejects_archive_end_without_preceding_end() {
    FrameValidator validator;

    vector<std::byte> const payload(4096 - neotape::fixed_header_size,
                                    std::byte{'b'});
    auto content_record =
        build_record(make_content_header(0, 0, 0, payload.size(), 0), payload);
    expect(
        !validator
             .validate(parse_header(content_record),
                       reinterpret_cast<const uint8_t *>(content_record.data()),
                       content_record.size())
             .has_value(),
        "unterminated content group should validate until archive_end");

    auto archive_end_record = build_record(make_archive_end_header(1));
    expect_validation_error(
        validator.validate(
            parse_header(archive_end_record),
            reinterpret_cast<const uint8_t *>(archive_end_record.data()),
            archive_end_record.size()),
        "archive_end before all slice channels reached END",
        "archive_end must follow a terminated channel group");
}

void test_validator_rejects_multiple_groups_in_same_slice() {
    FrameValidator validator;

    vector<std::byte> const payload = {std::byte{'c'}};
    auto first_record = build_record(
        make_content_header(0, 0, 0, payload.size(), neotape::frame_flag_end),
        payload);
    expect(
        !validator
             .validate(parse_header(first_record),
                       reinterpret_cast<const uint8_t *>(first_record.data()),
                       first_record.size())
             .has_value(),
        "first content group should validate");

    auto second_record = build_record(
        make_content_header(1, 0, 0, payload.size(), neotape::frame_flag_end),
        payload);
    expect_validation_error(
        validator.validate(
            parse_header(second_record),
            reinterpret_cast<const uint8_t *>(second_record.data()),
            second_record.size()),
        "CH_CONTENT frame after channel END",
        "same-slice second content group should be rejected");
}

void test_validator_seed_accepts_volume_local_start_and_rejects_gap() {
    FrameValidator validator;

    uint32_t const payload_capacity = 4096 - neotape::fixed_header_size;
    vector<std::byte> const full_payload(payload_capacity, std::byte{'s'});
    auto first_record = build_record(
        make_content_header(42, 3, 7, payload_capacity, 0), full_payload);
    FrameHeader const first_header = parse_header(first_record);
    validator.seed_for_stream_start(first_header);
    expect(
        !validator
             .validate(first_header,
                       reinterpret_cast<const uint8_t *>(first_record.data()),
                       first_record.size())
             .has_value(),
        "seeded validator should accept a non-zero volume-local start");

    vector<std::byte> const final_payload = {std::byte{'x'}};
    auto gap_record =
        build_record(make_content_header(44, 3, 8, final_payload.size(),
                                         neotape::frame_flag_end),
                     final_payload);
    expect_validation_error(
        validator.validate(parse_header(gap_record),
                           reinterpret_cast<const uint8_t *>(gap_record.data()),
                           gap_record.size()),
        "global_frame_seq_num 44 != expected 43",
        "seeded validator should reject a connection-local sequence gap");
}

void test_validator_accepts_interleaved_fec_group() {
    FrameValidator validator;
    constexpr uint32_t capacity = 4096 - neotape::fixed_header_size;
    vector<std::byte> first(capacity, std::byte{0x31});
    vector<std::byte> second(17, std::byte{0x72});

    vector<uint8_t> source_stream;
    source_stream.insert(source_stream.end(), capacity, 0x31);
    source_stream.insert(source_stream.end(), second.size(), 0x72);
    neotape::FecDescriptor descriptor;
    descriptor.source_content_frame_start = 0;
    descriptor.source_frame_count = 2;
    descriptor.source_stream_size = source_stream.size();
    descriptor.fec_group_blake3 =
        neotape::blake3_hash(source_stream.data(), source_stream.size());

    vector<neotape::FecShard> sources = {first, second};
    neotape::FecRepairShards const repair =
        neotape::encode_rs_32_4(sources, capacity);

    vector<vector<std::byte>> records;
    records.push_back(
        build_record(make_content_header(0, 0, 0, capacity,
                                         neotape::frame_flag_fec_protected),
                     first));
    records.push_back(
        build_record(make_content_header(1, 0, 1, second.size(),
                                         neotape::frame_flag_fec_protected |
                                             neotape::frame_flag_end),
                     second));
    for (uint16_t index = 0; index < neotape::fec_repair_shards; ++index) {
        descriptor.repair_index = index;
        records.push_back(build_record(
            make_fec_header(2 + index, index, descriptor,
                            index + 1 == neotape::fec_repair_shards),
            repair[index]));
    }
    records.push_back(build_record(make_archive_end_header(6)));

    for (std::size_t i = 0; i < records.size(); ++i) {
        auto error = validator.validate(
            parse_header(records[i]),
            reinterpret_cast<const uint8_t *>(records[i].data()),
            records[i].size());
        if (error.has_value()) {
            fail("valid interleaved FEC group rejected at record " +
                 std::to_string(i) + ": " + *error);
        }
    }
    expect(validator.saw_archive_end,
           "FEC archive should finish with archive_end");
}

void test_salvage_relaxes_consistency_but_keeps_integrity() {
    FrameValidator validator;
    vector<std::byte> payload = {std::byte{'s'}, std::byte{'a'},
                                 std::byte{'v'}};
    FrameHeader header = make_content_header(99, 42, 17, payload.size(),
                                             neotape::frame_flag_end);
    header.archive_uuid = "00000000-0000-4000-8000-999999999999";
    auto record = build_record(header, payload);

    RestoreFrameValidation result = validator.validate_salvage_frame(
        parse_header(record), reinterpret_cast<const uint8_t *>(record.data()),
        record.size());
    expect_restore_status(result, RestoreFrameValidationStatus::ok,
                          "salvage should ignore archive/sequence consistency");

    record[neotape::fixed_header_size] ^= std::byte{1};
    result = validator.validate_salvage_frame(
        parse_header(record), reinterpret_cast<const uint8_t *>(record.data()),
        record.size());
    expect_restore_status(result, RestoreFrameValidationStatus::fatal,
                          "salvage must retain frame integrity checks");
    expect_contains(result.message, "frame hash mismatch",
                    "salvage integrity failure text");
}

void test_seeded_validator_accepts_fec_group_split_across_volumes() {
    FrameValidator validator;
    constexpr uint32_t capacity = 4096 - neotape::fixed_header_size;
    vector<std::byte> payload(capacity, std::byte{0x4f});

    auto first =
        build_record(make_content_header(8, 0, 8, capacity,
                                         neotape::frame_flag_fec_protected),
                     payload);
    FrameHeader const first_header = parse_header(first);
    validator.seed_for_stream_start(first_header);
    expect(!validator
                .validate(first_header,
                          reinterpret_cast<const uint8_t *>(first.data()),
                          first.size())
                .has_value(),
           "seeded protected content should validate");

    for (uint64_t sequence = 9; sequence < 32; ++sequence) {
        uint64_t flags = neotape::frame_flag_fec_protected;
        if (sequence == 31) {
            flags |= neotape::frame_flag_end;
        }
        auto record = build_record(
            make_content_header(sequence, 0, sequence, capacity, flags),
            payload);
        auto error = validator.validate(
            parse_header(record),
            reinterpret_cast<const uint8_t *>(record.data()), record.size());
        if (error.has_value()) {
            fail("seeded split FEC content rejected: " + *error);
        }
    }

    neotape::FecDescriptor descriptor;
    descriptor.source_content_frame_start = 0;
    descriptor.source_frame_count = 32;
    descriptor.source_stream_size = 32ULL * capacity;
    vector<std::byte> repair(capacity, std::byte{0});
    for (uint16_t index = 0; index < neotape::fec_repair_shards; ++index) {
        descriptor.repair_index = index;
        auto record = build_record(
            make_fec_header(32 + index, index, descriptor,
                            index + 1 == neotape::fec_repair_shards),
            repair);
        auto error = validator.validate(
            parse_header(record),
            reinterpret_cast<const uint8_t *>(record.data()), record.size());
        if (error.has_value()) {
            fail("seeded split FEC repair rejected: " + *error);
        }
    }
}

} // namespace

int main() {
    test_restore_mode_metadata_hash_warning();
    test_restore_mode_metadata_structural_failure_is_fatal();
    test_validator_rejects_first_frame_with_nonzero_channel_frame_seq();
    test_validator_rejects_new_slice_without_channel_frame_seq_reset();
    test_validator_rejects_archive_end_without_preceding_end();
    test_validator_rejects_multiple_groups_in_same_slice();
    test_validator_seed_accepts_volume_local_start_and_rejects_gap();
    test_validator_accepts_interleaved_fec_group();
    test_salvage_relaxes_consistency_but_keeps_integrity();
    test_seeded_validator_accepts_fec_group_split_across_volumes();
    std::cout << "test_validate: ok\n";
    return 0;
}
