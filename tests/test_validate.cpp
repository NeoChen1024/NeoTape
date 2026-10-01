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

vector<std::byte> build_record(FrameHeader header,
                               const vector<std::byte> &payload = {}) {
    REQUIRE(payload.size() == header.frame_payload_size);
    vector<std::byte> record(neotape::decoded_block_size(header), std::byte{0});
    std::ranges::copy(payload, record.begin() + neotape::fixed_header_size);
    neotape::finalize_record(header, record);
    return record;
}

const uint8_t *bytes(const vector<std::byte> &record) {
    return reinterpret_cast<const uint8_t *>(record.data());
}

neotape::CheckedFrame check(const vector<std::byte> &record) {
    return neotape::check_frame(bytes(record), record.size());
}

std::optional<string> feed(FrameValidator &validator,
                           const vector<std::byte> &record) {
    return validator.validate(check(record), bytes(record));
}

RestoreFrameValidation restore(FrameValidator &validator,
                               const vector<std::byte> &record) {
    return validator.validate_restore_frame(check(record), bytes(record));
}

RestoreFrameValidation salvage(FrameValidator &validator,
                               const vector<std::byte> &record) {
    return validator.validate_salvage_frame(check(record), bytes(record));
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

    RestoreFrameValidation const metadata_result =
        restore(validator, metadata_record);
    REQUIRE(metadata_result.status == RestoreFrameValidationStatus::warning);
    REQUIRE_THAT(metadata_result.message, Catch::Matchers::ContainsSubstring(
                                              "metadata frame hash mismatch"));
    REQUIRE(validator.expected_global_frame_seq == 1);

    vector<std::byte> const content_payload = {std::byte{'o'}, std::byte{'k'}};
    auto content_record =
        build_record(make_content_header(1, 0, 0, content_payload.size(),
                                         neotape::frame_flag_end),
                     content_payload);
    RestoreFrameValidation const content_result =
        restore(validator, content_record);
    REQUIRE(content_result.status == RestoreFrameValidationStatus::ok);

    auto archive_end_record = build_record(make_archive_end_header(2));
    RestoreFrameValidation const archive_end_result =
        restore(validator, archive_end_record);
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
    auto first_result = restore(validator, first_record);
    REQUIRE(first_result.status == RestoreFrameValidationStatus::ok);

    FrameHeader second_header = make_metadata_header(
        1, 1, 0, metadata_payload.size(), neotape::frame_flag_end);
    second_header.archive_uuid = "00000000-0000-4000-8000-000000000999";
    auto second_record = build_record(second_header, metadata_payload);
    RestoreFrameValidation const second_result =
        restore(validator, second_record);
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
    auto const error = feed(validator, record);
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
    REQUIRE(!feed(validator, first_record).has_value());

    auto second_record = build_record(
        make_content_header(1, 1, 1, payload.size(), neotape::frame_flag_end),
        payload);
    auto const error = feed(validator, second_record);
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
    REQUIRE(!feed(validator, content_record).has_value());

    auto archive_end_record = build_record(make_archive_end_header(1));
    auto const error = feed(validator, archive_end_record);
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
    REQUIRE(!feed(validator, first_record).has_value());

    auto second_record = build_record(
        make_content_header(1, 0, 0, payload.size(), neotape::frame_flag_end),
        payload);
    auto const error = feed(validator, second_record);
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
    FrameHeader const first_header = check(first_record).header;
    validator.seed_for_stream_start(first_header);
    REQUIRE(!feed(validator, first_record).has_value());

    vector<std::byte> const final_payload = {std::byte{'x'}};
    auto gap_record =
        build_record(make_content_header(44, 3, 8, final_payload.size(),
                                         neotape::frame_flag_end),
                     final_payload);
    auto const error = feed(validator, gap_record);
    REQUIRE(error.has_value());
    REQUIRE_THAT(*error, Catch::Matchers::ContainsSubstring(
                             "global_frame_seq_num 44 != expected 43"));
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

    RestoreFrameValidation result = salvage(validator, record);
    REQUIRE(result.status == RestoreFrameValidationStatus::ok);

    record[neotape::fixed_header_size] ^= std::byte{1};
    result = salvage(validator, record);
    REQUIRE(result.status == RestoreFrameValidationStatus::fatal);
    REQUIRE_THAT(result.message,
                 Catch::Matchers::ContainsSubstring("frame hash mismatch"));
}

} // namespace

TEST_CASE("validate: replayed end marker does not reopen archive",
          "[unit][validation][replay]") {
    FrameValidator validator;
    auto content =
        build_record(make_content_header(0, 0, 0, 0, neotape::frame_flag_end));
    auto ending = build_record(make_archive_end_header(1));
    for (auto const &record : {content, ending, content, ending}) {
        REQUIRE_FALSE(feed(validator, record));
    }
    REQUIRE(validator.last_was_replay);
    REQUIRE(validator.expected_global_frame_seq == 2);
    REQUIRE(validator.saw_archive_end);
    auto appended =
        build_record(make_content_header(2, 1, 0, 0, neotape::frame_flag_end));
    REQUIRE(feed(validator, appended).has_value());
}

TEST_CASE("validate: block size cannot change on a new volume",
          "[unit][validation]") {
    FrameValidator validator;
    auto first =
        build_record(make_content_header(0, 0, 0, 0, neotape::frame_flag_end));
    REQUIRE_FALSE(feed(validator, first));
    auto next_header = make_content_header(1, 1, 0, 0, neotape::frame_flag_end);
    next_header.volume_seq_num = 2;
    next_header.volume_block_size_kib = 8;
    auto next = build_record(next_header);
    REQUIRE(feed(validator, next).has_value());
}

TEST_CASE("validate: volume can begin at every content position",
          "[unit][validation]") {
    constexpr uint32_t block_size = 4096, capacity = block_size - 512;
    auto identity = make_content_header(0, 0, 0, 0, 0);
    neotape::ContentFrameBuilder builder(block_size, identity.archive_uuid,
                                         "seed");
    std::vector<std::byte> source(capacity * 70 + 123, std::byte{0x5a});
    auto records = builder.feed(source);
    auto tail = builder.flush();
    for (auto &frame : tail)
        records.push_back(std::move(frame));
    for (auto &frame : records) {
        auto header = check(frame.record).header;
        neotape::finalize_record(header, frame.record);
    }
    auto ending = neotape::build_archive_end_record(
        block_size, 1, identity.archive_uuid, "seed",
        builder.next_global_seq_num());
    for (size_t begin = 1; begin < records.size(); ++begin) {
        CAPTURE(begin);
        FrameValidator validator;
        validator.seed_for_stream_start(check(records[begin].record).header);
        for (size_t index = begin; index < records.size(); ++index) {
            CAPTURE(index);
            auto const &record = records[index].record;
            auto error = feed(validator, record);
            CAPTURE(error);
            REQUIRE_FALSE(error);
        }
        REQUIRE_FALSE(feed(validator, ending));
    }
}
