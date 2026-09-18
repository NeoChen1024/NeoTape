#include "neotape/fec.hpp"
#include "neotape/format.hpp"
#include "neotape/frame_builder.hpp"
#include "neotape/validate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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
    REQUIRE(payload.size() == header.frame_payload_size);

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

TEST_CASE("validate: restore mode metadata hash warning",
          "[unit][validation]") {
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
    REQUIRE(metadata_result.status == RestoreFrameValidationStatus::warning);
    REQUIRE_THAT(metadata_result.message, Catch::Matchers::ContainsSubstring(
                                              "metadata frame hash mismatch"));
    REQUIRE(validator.expected_global_frame_seq == 1);

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
    REQUIRE(content_result.status == RestoreFrameValidationStatus::ok);

    auto archive_end_record = build_record(make_archive_end_header(2));
    FrameHeader const archive_end_header = parse_header(archive_end_record);
    RestoreFrameValidation const archive_end_result =
        validator.validate_restore_frame(
            archive_end_header,
            reinterpret_cast<const uint8_t *>(archive_end_record.data()),
            archive_end_record.size());
    REQUIRE(archive_end_result.status == RestoreFrameValidationStatus::ok);
    REQUIRE(validator.saw_archive_end);
}

TEST_CASE("validate: restore mode metadata structural failure is fatal",
          "[unit][validation]") {
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
    REQUIRE(first_result.status == RestoreFrameValidationStatus::ok);

    FrameHeader second_header = make_metadata_header(
        1, 1, 0, metadata_payload.size(), neotape::frame_flag_end);
    second_header.archive_uuid = "00000000-0000-4000-8000-000000000999";
    auto second_record = build_record(second_header, metadata_payload);
    RestoreFrameValidation const second_result =
        validator.validate_restore_frame(
            parse_header(second_record),
            reinterpret_cast<const uint8_t *>(second_record.data()),
            second_record.size());
    REQUIRE(second_result.status == RestoreFrameValidationStatus::fatal);
    REQUIRE_THAT(second_result.message,
                 Catch::Matchers::ContainsSubstring("archive_uuid mismatch"));
}

TEST_CASE(
    "validate: validator rejects first frame with nonzero channel frame seq",
    "[unit][validation]") {
    FrameValidator validator;

    vector<std::byte> const payload = {std::byte{'x'}};
    auto record = build_record(
        make_content_header(0, 0, 1, payload.size(), neotape::frame_flag_end),
        payload);
    auto const error = validator.validate(
        parse_header(record), reinterpret_cast<const uint8_t *>(record.data()),
        record.size());
    REQUIRE(error.has_value());
    REQUIRE_THAT(*error, Catch::Matchers::ContainsSubstring(
                             "channel_frame_seq_num 1 != expected 0"));
}

TEST_CASE(
    "validate: validator rejects new slice without channel frame seq reset",
    "[unit][validation]") {
    FrameValidator validator;

    vector<std::byte> const payload = {std::byte{'a'}};
    auto first_record = build_record(
        make_content_header(0, 0, 0, payload.size(), neotape::frame_flag_end),
        payload);
    REQUIRE(
        !validator
             .validate(parse_header(first_record),
                       reinterpret_cast<const uint8_t *>(first_record.data()),
                       first_record.size())
             .has_value());

    auto second_record = build_record(
        make_content_header(1, 1, 1, payload.size(), neotape::frame_flag_end),
        payload);
    auto const error = validator.validate(
        parse_header(second_record),
        reinterpret_cast<const uint8_t *>(second_record.data()),
        second_record.size());
    REQUIRE(error.has_value());
    REQUIRE_THAT(*error, Catch::Matchers::ContainsSubstring(
                             "channel_frame_seq_num 1 != expected 0"));
}

TEST_CASE("validate: validator rejects archive end without preceding end",
          "[unit][validation]") {
    FrameValidator validator;

    vector<std::byte> const payload(4096 - neotape::fixed_header_size,
                                    std::byte{'b'});
    auto content_record =
        build_record(make_content_header(0, 0, 0, payload.size(), 0), payload);
    REQUIRE(
        !validator
             .validate(parse_header(content_record),
                       reinterpret_cast<const uint8_t *>(content_record.data()),
                       content_record.size())
             .has_value());

    auto archive_end_record = build_record(make_archive_end_header(1));
    auto const error = validator.validate(
        parse_header(archive_end_record),
        reinterpret_cast<const uint8_t *>(archive_end_record.data()),
        archive_end_record.size());
    REQUIRE(error.has_value());
    REQUIRE_THAT(*error,
                 Catch::Matchers::ContainsSubstring(
                     "archive_end before all slice channels reached END"));
}

TEST_CASE("validate: validator rejects multiple groups in same slice",
          "[unit][validation]") {
    FrameValidator validator;

    vector<std::byte> const payload = {std::byte{'c'}};
    auto first_record = build_record(
        make_content_header(0, 0, 0, payload.size(), neotape::frame_flag_end),
        payload);
    REQUIRE(
        !validator
             .validate(parse_header(first_record),
                       reinterpret_cast<const uint8_t *>(first_record.data()),
                       first_record.size())
             .has_value());

    auto second_record = build_record(
        make_content_header(1, 0, 0, payload.size(), neotape::frame_flag_end),
        payload);
    auto const error = validator.validate(
        parse_header(second_record),
        reinterpret_cast<const uint8_t *>(second_record.data()),
        second_record.size());
    REQUIRE(error.has_value());
    REQUIRE_THAT(*error, Catch::Matchers::ContainsSubstring(
                             "CH_CONTENT frame after channel END"));
}

TEST_CASE("validate: validator seed accepts volume local start and rejects gap",
          "[unit][validation]") {
    FrameValidator validator;

    uint32_t const payload_capacity = 4096 - neotape::fixed_header_size;
    vector<std::byte> const full_payload(payload_capacity, std::byte{'s'});
    auto first_record = build_record(
        make_content_header(42, 3, 7, payload_capacity, 0), full_payload);
    FrameHeader const first_header = parse_header(first_record);
    validator.seed_for_stream_start(first_header);
    REQUIRE(
        !validator
             .validate(first_header,
                       reinterpret_cast<const uint8_t *>(first_record.data()),
                       first_record.size())
             .has_value());

    vector<std::byte> const final_payload = {std::byte{'x'}};
    auto gap_record =
        build_record(make_content_header(44, 3, 8, final_payload.size(),
                                         neotape::frame_flag_end),
                     final_payload);
    auto const error =
        validator.validate(parse_header(gap_record),
                           reinterpret_cast<const uint8_t *>(gap_record.data()),
                           gap_record.size());
    REQUIRE(error.has_value());
    REQUIRE_THAT(*error, Catch::Matchers::ContainsSubstring(
                             "global_frame_seq_num 44 != expected 43"));
}

TEST_CASE("validate: validator accepts interleaved fec group",
          "[unit][validation]") {
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
        CAPTURE(error);
        REQUIRE_FALSE(error.has_value());
    }
    REQUIRE(validator.saw_archive_end);

    FrameValidator unavailable_validator;
    records[1][neotape::fixed_header_size] ^= std::byte{1};
    for (std::size_t i = 0; i < records.size(); ++i) {
        auto error = unavailable_validator.validate(
            parse_header(records[i]),
            reinterpret_cast<const uint8_t *>(records[i].data()),
            records[i].size(), i == 1);
        CAPTURE(error);
        REQUIRE_FALSE(error.has_value());
    }
}

TEST_CASE("validate: salvage relaxes consistency but keeps integrity",
          "[unit][validation]") {
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
    REQUIRE(result.status == RestoreFrameValidationStatus::ok);

    record[neotape::fixed_header_size] ^= std::byte{1};
    result = validator.validate_salvage_frame(
        parse_header(record), reinterpret_cast<const uint8_t *>(record.data()),
        record.size());
    REQUIRE(result.status == RestoreFrameValidationStatus::fatal);
    REQUIRE_THAT(result.message,
                 Catch::Matchers::ContainsSubstring("frame hash mismatch"));
}

TEST_CASE("validate: seeded validator accepts fec group split across volumes",
          "[unit][validation]") {
    FrameValidator validator;
    constexpr uint32_t capacity = 4096 - neotape::fixed_header_size;
    vector<std::byte> payload(capacity, std::byte{0x4f});

    auto first =
        build_record(make_content_header(8, 0, 8, capacity,
                                         neotape::frame_flag_fec_protected),
                     payload);
    FrameHeader const first_header = parse_header(first);
    validator.seed_for_stream_start(first_header);
    REQUIRE(!validator
                 .validate(first_header,
                           reinterpret_cast<const uint8_t *>(first.data()),
                           first.size())
                 .has_value());

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
        CAPTURE(error);
        REQUIRE_FALSE(error.has_value());
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
        CAPTURE(error);
        REQUIRE_FALSE(error.has_value());
    }
}

} // namespace

TEST_CASE("validate: replayed end marker does not reopen archive",
          "[unit][validation][replay]") {
    FrameValidator validator;
    auto content =
        build_record(make_content_header(0, 0, 0, 0, neotape::frame_flag_end));
    auto ending = build_record(make_archive_end_header(1));
    for (auto const &record : {content, ending, content, ending}) {
        REQUIRE_FALSE(validator.validate(
            parse_header(record),
            reinterpret_cast<const uint8_t *>(record.data()), record.size()));
    }
    REQUIRE(validator.last_was_replay);
    REQUIRE(validator.expected_global_frame_seq == 2);
    REQUIRE(validator.saw_archive_end);
    auto appended =
        build_record(make_content_header(2, 1, 0, 0, neotape::frame_flag_end));
    REQUIRE(validator
                .validate(parse_header(appended),
                          reinterpret_cast<const uint8_t *>(appended.data()),
                          appended.size())
                .has_value());
}

TEST_CASE("validate: unsigned replay must still have zero signature bytes",
          "[unit][validation][replay]") {
    FrameValidator validator;
    auto record =
        build_record(make_content_header(0, 0, 0, 0, neotape::frame_flag_end));
    REQUIRE_FALSE(validator.validate(
        parse_header(record), reinterpret_cast<const uint8_t *>(record.data()),
        record.size()));
    record[408] = std::byte{1}; // Signature is excluded from the frame hash.
    REQUIRE(validator
                .validate(parse_header(record),
                          reinterpret_cast<const uint8_t *>(record.data()),
                          record.size())
                .has_value());
}

TEST_CASE("validate: block size cannot change on a new volume",
          "[unit][validation]") {
    FrameValidator validator;
    auto first =
        build_record(make_content_header(0, 0, 0, 0, neotape::frame_flag_end));
    REQUIRE_FALSE(validator.validate(
        parse_header(first), reinterpret_cast<const uint8_t *>(first.data()),
        first.size()));
    auto next_header = make_content_header(1, 1, 0, 0, neotape::frame_flag_end);
    next_header.volume_seq_num = 2;
    next_header.volume_block_size_kib = 8;
    auto next = build_record(next_header);
    REQUIRE(validator
                .validate(parse_header(next),
                          reinterpret_cast<const uint8_t *>(next.data()),
                          next.size())
                .has_value());
}

TEST_CASE(
    "validate: recovery requires sequence evidence for missing repair indices",
    "[unit][validation][fec]") {
    FrameValidator validator;
    validator.recover_missing_fec = true;
    vector<std::byte> payload = {std::byte{'x'}};
    auto source =
        build_record(make_content_header(0, 0, 0, 1,
                                         neotape::frame_flag_fec_protected |
                                             neotape::frame_flag_end),
                     payload);
    REQUIRE_FALSE(validator.validate(
        parse_header(source), reinterpret_cast<const uint8_t *>(source.data()),
        source.size()));
    neotape::FecDescriptor descriptor;
    descriptor.source_frame_count = 1;
    descriptor.source_stream_size = 1;
    descriptor.fec_group_blake3 = neotape::blake3_hash(
        reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
    descriptor.repair_index = 2;
    // No global/channel gap exists to account for missing repairs 0 and 1.
    auto repair = build_record(make_fec_header(1, 0, descriptor, false),
                               vector<std::byte>(3584));
    REQUIRE(validator
                .validate(parse_header(repair),
                          reinterpret_cast<const uint8_t *>(repair.data()),
                          repair.size())
                .has_value());
}

TEST_CASE("validate: volume can begin at every position of later FEC groups",
          "[unit][validation][fec]") {
    constexpr uint32_t block_size = 4096, capacity = block_size - 512;
    auto identity = make_content_header(0, 0, 0, 0, 0);
    neotape::ContentFrameBuilder builder(block_size, identity.archive_uuid,
                                         "seed", true);
    std::vector<std::byte> source(capacity * 70 + 123, std::byte{0x5a});
    auto records = builder.feed(source);
    auto tail = builder.flush();
    for (auto &frame : tail)
        records.push_back(std::move(frame));
    for (auto &frame : records) {
        auto header = parse_header(frame.record);
        neotape::finalize_record(header, frame.record);
    }
    auto ending = neotape::build_archive_end_record(
        block_size, 1, identity.archive_uuid, "seed",
        builder.next_global_seq_num());
    for (size_t begin = 1; begin < records.size(); ++begin) {
        CAPTURE(begin);
        FrameValidator validator;
        validator.seed_for_stream_start(parse_header(records[begin].record));
        for (size_t index = begin; index < records.size(); ++index) {
            CAPTURE(index);
            auto const &record = records[index].record;
            auto error = validator.validate(
                parse_header(record),
                reinterpret_cast<const uint8_t *>(record.data()),
                record.size());
            CAPTURE(error);
            REQUIRE_FALSE(error);
        }
        REQUIRE_FALSE(validator.validate(
            parse_header(ending),
            reinterpret_cast<const uint8_t *>(ending.data()), ending.size()));
    }
}
