#include "neotape/validate.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace neotape {

using std::format;
using std::string;

namespace {

RestoreFrameValidation
make_restore_validation(RestoreFrameValidationStatus status,
                        std::string message = {}) {
    return RestoreFrameValidation{status, std::move(message)};
}

} // namespace

// -----------------------------------------------------------------------
// FrameValidator — archive-level state machine
// -----------------------------------------------------------------------

void FrameValidator::seed_for_stream_start(const FrameHeader &header) {
    reset();
    expected_global_frame_seq = header.global_frame_seq_num;
    current_slice_seq_num = header.slice_seq_num;
    expected_channel_frame_seq_num = header.channel_frame_seq_num;
    last_channel_type = header.channel_type;
    current_phase =
        header.channel_type == ChannelType::CH_CONTENT    ? Phase::content
        : header.channel_type == ChannelType::CH_METADATA ? Phase::metadata
                                                          : Phase::none;
    saw_any_frame = true;
    last_frame_had_end = header.channel_type == ChannelType::ARCHIVE_END;
}

std::optional<string> FrameValidator::validate(const FrameHeader &header,
                                               const uint8_t *raw_data,
                                               std::size_t record_size,
                                               bool skip_hash) {
    // Reject any frame presented after archive_end.
    if (saw_archive_end) {
        return format("frame after archive_end at global_seq={}",
                      header.global_frame_seq_num);
    }

    // Capture whether we had a previous frame before this one is processed.
    bool const had_previous_frame = saw_any_frame;

    const uint32_t block_size = decoded_block_size(header);

    // --- record size match ---
    if (record_size != block_size) {
        return format("record size {} != decoded block size {}", record_size,
                      block_size);
    }

    // --- frame_hash (may be skipped for advisory metadata frames) ---
    if (!skip_hash) {
        if (!verify_frame_hash(raw_data, record_size, header.frame_hash)) {
            return format("frame hash mismatch at global_seq={}",
                          header.global_frame_seq_num);
        }
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
        if (had_previous_frame && !last_frame_had_end) {
            return format("archive_end without END flag on preceding frame "
                          "at global_seq={}",
                          header.global_frame_seq_num);
        }
        if (!has_frame_flag_clean_end(header.flags)) {
            return "archive_end frame missing CLEAN_END";
        }
        if (header.slice_seq_num != 0) {
            return format("archive_end slice_seq_num {} != 0",
                          header.slice_seq_num);
        }
        if (header.channel_frame_seq_num != 0) {
            return format("archive_end channel_frame_seq_num {} != 0",
                          header.channel_frame_seq_num);
        }
        saw_archive_end = true;
        return std::nullopt;
    }

    // --- slice_seq_num ---
    bool slice_changed = false;
    if (!saw_any_frame) {
        if (header.slice_seq_num != 0) {
            return format("first frame slice_seq_num {} != 0",
                          header.slice_seq_num);
        }
        current_slice_seq_num = 0;
        saw_any_frame = true;
    }

    if (header.slice_seq_num != current_slice_seq_num) {
        if (header.slice_seq_num != current_slice_seq_num + 1) {
            return format("slice_seq_num {} jumped from {}",
                          header.slice_seq_num, current_slice_seq_num);
        }
        // Previous slice must have ended cleanly.
        if (had_previous_frame && !last_frame_had_end) {
            return format("slice transition without END flag at global_seq={}",
                          header.global_frame_seq_num);
        }
        slice_changed = true;
        current_slice_seq_num = header.slice_seq_num;
        current_phase = Phase::none;
        expected_channel_frame_seq_num = 0;
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

    // --- END flag and channel-group boundaries ---
    bool const channel_changed = had_previous_frame && !slice_changed &&
                                 header.channel_type != last_channel_type;

    if (had_previous_frame && !slice_changed &&
        header.channel_type == last_channel_type && last_frame_had_end) {
        return format("multiple {} groups in slice {}",
                      channel_type_name(header.channel_type),
                      header.slice_seq_num);
    }

    // Previous channel group must have ended before a new one starts.
    if (channel_changed && !last_frame_had_end) {
        return format("channel group transition without END flag at "
                      "global_seq={}",
                      header.global_frame_seq_num);
    }

    bool const new_group =
        !had_previous_frame || slice_changed || channel_changed;
    // --- channel_frame_seq_num ---
    if (new_group) {
        if (header.channel_frame_seq_num != 0) {
            return format("channel_frame_seq_num {} != 0 at start of new "
                          "channel group",
                          header.channel_frame_seq_num);
        }
    } else {
        if (header.channel_frame_seq_num != expected_channel_frame_seq_num) {
            return format("channel_frame_seq_num {} != expected {}",
                          header.channel_frame_seq_num,
                          expected_channel_frame_seq_num);
        }
    }

    if (has_frame_flag_end(header.flags)) {
        expected_channel_frame_seq_num = 0;
    } else {
        expected_channel_frame_seq_num = header.channel_frame_seq_num + 1;
    }
    last_channel_type = header.channel_type;
    last_frame_had_end = has_frame_flag_end(header.flags);

    return std::nullopt; // OK
}

RestoreFrameValidation
FrameValidator::validate_restore_frame(const FrameHeader &header,
                                       const uint8_t *raw_data,
                                       std::size_t record_size) {
    if (header.channel_type == ChannelType::CH_METADATA) {
        bool const hash_ok =
            verify_frame_hash(raw_data, record_size, header.frame_hash);
        if (auto err = validate(header, raw_data, record_size, true);
            err.has_value()) {
            return make_restore_validation(RestoreFrameValidationStatus::fatal,
                                           std::move(*err));
        }
        if (!hash_ok) {
            return make_restore_validation(
                RestoreFrameValidationStatus::warning,
                format("metadata frame hash mismatch at global_seq={}",
                       header.global_frame_seq_num));
        }
        return make_restore_validation(RestoreFrameValidationStatus::ok);
    }

    if (auto err = validate(header, raw_data, record_size); err.has_value()) {
        return make_restore_validation(RestoreFrameValidationStatus::fatal,
                                       std::move(*err));
    }
    return make_restore_validation(RestoreFrameValidationStatus::ok);
}

void FrameValidator::reset() {
    archive_uuid.clear();
    archive_label.clear();
    expected_global_frame_seq = 0;
    expected_volume_seq_num = 0;
    current_slice_seq_num = 0;
    volume_block_size = 0;
    last_channel_type = {};
    expected_channel_frame_seq_num = 0;
    current_phase = Phase::none;
    saw_first_volume_seq = false;
    saw_any_frame = false;
    saw_archive_end = false;
    last_frame_had_end = false;
}

} // namespace neotape
