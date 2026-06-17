#include "neotape/validate.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace neotape {

using std::format;
using std::string;

// -----------------------------------------------------------------------
// FrameValidator — archive-level state machine
// -----------------------------------------------------------------------

std::optional<string> FrameValidator::validate(const FrameHeader &header,
                                               const uint8_t *raw_data,
                                               std::size_t record_size) {
    const uint32_t block_size = decoded_block_size(header);

    // --- record size match ---
    if (record_size != block_size) {
        return format("record size {} != decoded block size {}", record_size,
                      block_size);
    }

    // --- frame_hash ---
    if (!verify_frame_hash(raw_data, record_size, header.frame_hash)) {
        return format("frame hash mismatch at global_seq={}",
                      header.global_frame_seq_num);
    }

    // --- SIGNED flag / signature consistency ---
    {
        bool const sig_set = std::ranges::any_of(
            header.signature, [](uint8_t b) { return b != 0; });
        if (has_frame_flag_signed(header.flags)) {
            if (!sig_set) {
                return "SIGNED flag set but signature bytes are all zero";
            }
        } else if (sig_set) {
            return "non-zero signature bytes without SIGNED flag";
        }
    }

    // --- volume_block_size consistency ---
    if (volume_block_size == 0) {
        volume_block_size = block_size;
    } else if (block_size != volume_block_size) {
        return format("volume block size changed from {} to {}",
                      volume_block_size, block_size);
    }

    // --- archive_uuid / archive_label ---
    if (archive_uuid.empty()) {
        archive_uuid = header.archive_uuid;
        archive_label = header.archive_label;
    } else {
        if (header.archive_uuid != archive_uuid) {
            return "archive_uuid mismatch";
        }
        if (header.archive_label != archive_label) {
            return "archive_label mismatch";
        }
    }

    // --- global_frame_seq_num ---
    if (header.global_frame_seq_num != expected_global_frame_seq) {
        return format("global_frame_seq_num {} != expected {}",
                      header.global_frame_seq_num, expected_global_frame_seq);
    }
    expected_global_frame_seq = header.global_frame_seq_num + 1;

    // --- volume_seq_num (advisory, monotonic, at most +1) ---
    if (!saw_first_volume_seq) {
        expected_volume_seq_num = header.volume_seq_num;
        saw_first_volume_seq = true;
    } else {
        if (header.volume_seq_num < expected_volume_seq_num) {
            return format("volume_seq_num {} went backward from {}",
                          header.volume_seq_num, expected_volume_seq_num);
        }
        if (header.volume_seq_num > expected_volume_seq_num + 1) {
            return format("volume_seq_num {} skipped ahead from {}",
                          header.volume_seq_num, expected_volume_seq_num);
        }
        expected_volume_seq_num = header.volume_seq_num;
    }

    // --- archive_end ---
    if (header.channel_type == ChannelType::ARCHIVE_END) {
        if (!has_frame_flag_clean_end(header.flags)) {
            return "archive_end frame missing CLEAN_END";
        }
        if (header.logical_slice_seq_num != 0) {
            return format("archive_end logical_slice_seq_num {} != 0",
                          header.logical_slice_seq_num);
        }
        saw_archive_end = true;
        return std::nullopt;
    }

    // --- logical_slice_seq_num ---
    if (!saw_any_frame) {
        if (header.logical_slice_seq_num != 1) {
            return format("first frame logical_slice_seq_num {} != 1",
                          header.logical_slice_seq_num);
        }
        current_slice_seq_num = 1;
        saw_any_frame = true;
    }

    if (header.logical_slice_seq_num != current_slice_seq_num) {
        if (header.logical_slice_seq_num != current_slice_seq_num + 1) {
            return format("logical_slice_seq_num {} jumped from {}",
                          header.logical_slice_seq_num, current_slice_seq_num);
        }
        current_slice_seq_num = header.logical_slice_seq_num;
        current_phase = Phase::none;
        expected_frame_seq_within_channel = 1;
    }

    // --- channel ordering: metadata before content within same slice ---
    if (header.channel_type == ChannelType::CH_CONTENT) {
        current_phase = Phase::content;
    } else if (header.channel_type == ChannelType::CH_METADATA) {
        if (current_phase == Phase::content) {
            return "metadata frame after content in same slice";
        }
        current_phase = Phase::metadata;
    }

    // --- frame_seq_num_within_channel ---
    if (header.channel_type != last_channel_type ||
        has_frame_flag_start(header.flags)) {
        if (header.frame_seq_num_within_channel != 1) {
            return format("frame_seq_num_within_channel {} != 1 at start "
                          "of new channel group",
                          header.frame_seq_num_within_channel);
        }
    } else {
        if (header.frame_seq_num_within_channel !=
            expected_frame_seq_within_channel) {
            return format("frame_seq_num_within_channel {} != expected {}",
                          header.frame_seq_num_within_channel,
                          expected_frame_seq_within_channel);
        }
    }

    if (has_frame_flag_end(header.flags)) {
        expected_frame_seq_within_channel = 1;
    } else {
        expected_frame_seq_within_channel =
            header.frame_seq_num_within_channel + 1;
    }
    last_channel_type = header.channel_type;

    return std::nullopt; // OK
}

void FrameValidator::reset() {
    archive_uuid.clear();
    archive_label.clear();
    expected_global_frame_seq = 1;
    expected_volume_seq_num = 0;
    current_slice_seq_num = 0;
    volume_block_size = 0;
    last_channel_type = {};
    expected_frame_seq_within_channel = 1;
    current_phase = Phase::none;
    saw_first_volume_seq = false;
    saw_any_frame = false;
    saw_archive_end = false;
}

} // namespace neotape
