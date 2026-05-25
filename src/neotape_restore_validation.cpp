#include "neotape/restore_validation.hpp"

#include <format>
#include <stdexcept>

namespace neotape {

using std::format;

void accept_restore_volume_header(const VolumeHeader &vh,
                                  RestoreValidationState &state) {
    if (!state.identity_established) {
        state.archive_uuid = vh.archive_uuid;
        state.archive_name = vh.archive_name;
        state.payload_profile = vh.payload_profile;
        state.volume_block_size = vh.volume_block_size;
        state.expected_volume_seq_num = vh.volume_seq_num;
        state.identity_established = true;
    }

    if (vh.archive_uuid != state.archive_uuid)
        throw std::runtime_error(
            format("volume archive uuid mismatch: expected {}, got {}",
                   state.archive_uuid, vh.archive_uuid));
    if (vh.volume_seq_num != state.expected_volume_seq_num)
        throw std::runtime_error(
            format("volume sequence mismatch: expected {}, got {}",
                   state.expected_volume_seq_num, vh.volume_seq_num));
    if (vh.volume_block_size != state.volume_block_size)
        throw std::runtime_error(
            format("volume block size mismatch: expected {}, got {}",
                   state.volume_block_size, vh.volume_block_size));
    if (vh.payload_profile != state.payload_profile)
        throw std::runtime_error("volume payload profile mismatch");

    state.current_volume_seq_num = vh.volume_seq_num;
    ++state.expected_volume_seq_num;
}

void validate_restore_frame_header(const FrameHeader &fh,
                                   const RestoreValidationState &state) {
    if (!state.identity_established)
        throw std::runtime_error("frame before volume header");
    if (fh.archive_uuid != state.archive_uuid)
        throw std::runtime_error(
            format("frame archive uuid mismatch: expected {}, got {}",
                   state.archive_uuid, fh.archive_uuid));
    if (fh.volume_seq_num != state.current_volume_seq_num)
        throw std::runtime_error(
            format("frame volume sequence mismatch: expected {}, got {}",
                   state.current_volume_seq_num, fh.volume_seq_num));
    if (fh.volume_block_size != state.volume_block_size)
        throw std::runtime_error(
            format("frame block size mismatch: expected {}, got {}",
                   state.volume_block_size, fh.volume_block_size));
    if (fh.global_frame_seq_num != state.expected_global_frame_seq_num)
        throw std::runtime_error(format(
            "frame sequence mismatch on volume {}: expected {}, got {}",
            state.current_volume_seq_num, state.expected_global_frame_seq_num,
            fh.global_frame_seq_num));

    bool start = (fh.flags & frame_flag_start) != 0;
    if (start &&
        fh.logical_slice_seq_num != state.expected_logical_slice_seq_num)
        throw std::runtime_error(format(
            "slice sequence mismatch on volume {}: expected {}, got {}",
            state.current_volume_seq_num, state.expected_logical_slice_seq_num,
            fh.logical_slice_seq_num));
}

void note_restore_frame_accepted(const FrameHeader &fh,
                                 RestoreValidationState &state) {
    bool start = (fh.flags & frame_flag_start) != 0;
    bool end = (fh.flags & frame_flag_end) != 0;
    if (start) {
        state.slice_open = true;
        state.current_slice_size = 0;
    }
    state.current_slice_size += fh.frame_payload_size;
    ++state.expected_global_frame_seq_num;
    if (end) {
        state.slice_open = false;
        ++state.expected_logical_slice_seq_num;
    }
}

void validate_restore_archive_end(const ArchiveEndHeader &ae,
                                  const RestoreValidationState &state) {
    if (state.slice_open)
        throw std::runtime_error("archive ended with open slice");
    if (!(ae.flags & archive_end_flag_clean_end))
        throw std::runtime_error("archive end missing CLEAN_END flag");
    if (ae.archive_uuid != state.archive_uuid)
        throw std::runtime_error(
            format("archive end uuid mismatch: expected {}, got {}",
                   state.archive_uuid, ae.archive_uuid));
    if (ae.last_global_frame_seq_num + 1 != state.expected_global_frame_seq_num)
        throw std::runtime_error(
            format("archive end frame seq mismatch: declared {} expected {}",
                   ae.last_global_frame_seq_num,
                   state.expected_global_frame_seq_num - 1));
}

} // namespace neotape
