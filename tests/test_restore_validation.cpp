#include "neotape/restore_validation.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool cond, const std::string &msg) {
    if (!cond) {
        std::cerr << "test_restore_validation: " << msg << "\n";
        std::exit(1);
    }
}

neotape::VolumeHeader volume(std::string uuid, uint64_t seq) {
    neotape::VolumeHeader vh;
    vh.volume_block_size = 4096;
    vh.archive_uuid = std::move(uuid);
    vh.archive_name = "restore-test";
    vh.volume_seq_num = seq;
    vh.payload_profile = neotape::PayloadProfile::pax;
    vh.volume_write_at_utc = "2026-05-25T00:00:00Z";
    return vh;
}

neotape::FrameHeader frame(std::string uuid, uint64_t volume_seq,
                           uint64_t global_seq, uint64_t slice_seq) {
    neotape::FrameHeader fh;
    fh.volume_block_size = 4096;
    fh.archive_uuid = std::move(uuid);
    fh.archive_name = "restore-test";
    fh.volume_seq_num = volume_seq;
    fh.logical_slice_seq_num = slice_seq;
    fh.global_frame_seq_num = global_seq;
    fh.frame_seq_num_within_slice = 1;
    fh.flags = neotape::frame_flag_start;
    return fh;
}

template <typename Fn> bool throws_with(const std::string &needle, Fn fn) {
    try {
        fn();
    } catch (const std::exception &e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

void test_wrong_archive_rejected() {
    neotape::RestoreValidationState state;
    neotape::accept_restore_volume_header(
        volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1), state);
    require(throws_with("archive uuid mismatch", [&] {
                neotape::accept_restore_volume_header(
                    volume("ffffffff-bbbb-cccc-dddd-eeeeeeeeeeee", 2), state);
            }),
            "wrong archive volume rejected");
}

void test_wrong_volume_sequence_rejected() {
    neotape::RestoreValidationState state;
    neotape::accept_restore_volume_header(
        volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1), state);
    require(throws_with("volume sequence mismatch", [&] {
                neotape::accept_restore_volume_header(
                    volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 3), state);
            }),
            "wrong volume sequence rejected");
}

void test_frame_sequence_rejected() {
    neotape::RestoreValidationState state;
    neotape::accept_restore_volume_header(
        volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1), state);
    require(throws_with("frame sequence mismatch", [&] {
                neotape::validate_restore_frame_header(
                    frame("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1, 2, 1),
                    state);
            }),
            "frame sequence mismatch rejected");
}

void test_slice_sequence_rejected() {
    neotape::RestoreValidationState state;
    neotape::accept_restore_volume_header(
        volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1), state);
    require(throws_with("slice sequence mismatch", [&] {
                neotape::validate_restore_frame_header(
                    frame("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1, 1, 2),
                    state);
            }),
            "slice sequence mismatch rejected");
}

} // namespace

int main() {
    test_wrong_archive_rejected();
    test_wrong_volume_sequence_rejected();
    test_frame_sequence_rejected();
    test_slice_sequence_rejected();
    return 0;
}
