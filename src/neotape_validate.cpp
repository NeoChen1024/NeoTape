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

std::size_t channel_index(ChannelType type) {
    switch (type) {
    case ChannelType::CH_CONTENT:
        return 0;
    case ChannelType::CH_METADATA:
        return 1;
    case ChannelType::ARCHIVE_END:
        break;
    }
    throw std::runtime_error("archive_end has no slice channel index");
}

bool all_seen_channels_ended(const FrameValidator &validator) {
    for (std::size_t i = 0; i < validator.channel_seen.size(); ++i) {
        if (validator.channel_seen[i] && !validator.channel_ended[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

// -----------------------------------------------------------------------
// FrameValidator — archive-level state machine
// -----------------------------------------------------------------------

void FrameValidator::seed_for_stream_start(const FrameHeader &header) {
    reset();
    expected_global_frame_seq = header.global_frame_seq_num;
    current_slice_seq_num = header.slice_seq_num;
    stream_start_seeded = true;
    if (header.channel_type != ChannelType::ARCHIVE_END)
        next_channel_seq[channel_index(header.channel_type)] =
            header.channel_frame_seq_num;
}

void FrameValidator::remember_record(const FrameHeader &header,
                                     const uint8_t *data, std::size_t size,
                                     bool skip_hash) {
    if (skip_hash)
        return;
    replay_history_.emplace_back(header.global_frame_seq_num,
                                 compute_replay_hash(data, size));
    constexpr std::size_t retry_history_records = 8192;
    if (replay_history_.size() > retry_history_records)
        replay_history_.pop_front();
}

void FrameValidator::begin_connection() { replay_next_.reset(); }

std::optional<string> FrameValidator::check_replay(const FrameHeader &header,
                                                   const uint8_t *raw_data,
                                                   std::size_t record_size,
                                                   bool skip_hash) {
    if (header.global_frame_seq_num < expected_global_frame_seq) {
        if (skip_hash ||
            !verify_frame_hash(raw_data, record_size, header.frame_hash))
            return "replay failed frame integrity";
        auto const found =
            std::ranges::find_if(replay_history_, [&](const auto &entry) {
                return entry.first == header.global_frame_seq_num;
            });
        if (found == replay_history_.end())
            return "cannot verify replay: prior comparison state unavailable";
        if (found->second != compute_replay_hash(raw_data, record_size))
            return "conflicting replay record";
        if (replay_next_ && *replay_next_ != header.global_frame_seq_num)
            return "non-contiguous replay suffix";
        replay_next_ = header.global_frame_seq_num + 1;
        if (*replay_next_ == expected_global_frame_seq)
            replay_next_.reset();
        last_was_replay = true;
        return std::nullopt;
    }
    if (replay_next_)
        return "new frame before replay suffix completed";
    return std::nullopt;
}

std::optional<string> FrameValidator::validate(const FrameHeader &header,
                                               const uint8_t *raw_data,
                                               std::size_t record_size,
                                               bool skip_hash) {
    last_was_replay = false;
    bool const signature_present =
        std::ranges::any_of(header.signature, [](uint8_t b) { return b != 0; });
    if (signature_present != has_frame_flag_signed(header.flags))
        return "SIGNED flag and signature bytes are inconsistent";
    if (auto error = check_replay(header, raw_data, record_size, skip_hash))
        return error;
    if (last_was_replay)
        return std::nullopt;
    // Reject any new logical frame presented after archive_end.
    if (saw_archive_end) {
        return format("frame after archive_end at global_seq={}",
                      header.global_frame_seq_num);
    }

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

    // Volume ordinals are advisory; gaps and backward values do not change
    // archive identity or logical continuity, so volume_seq_num is unchecked.

    // --- archive_end ---
    if (header.channel_type == ChannelType::ARCHIVE_END) {
        if (had_previous_frame && !all_seen_channels_ended(*this)) {
            return format("archive_end before all slice channels reached END "
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
        remember_record(header, raw_data, record_size, skip_hash);
        return std::nullopt;
    }

    // --- slice_seq_num ---
    if (!saw_any_frame) {
        if (!stream_start_seeded && header.slice_seq_num != 0) {
            return format("first frame slice_seq_num {} != 0",
                          header.slice_seq_num);
        }
        current_slice_seq_num = header.slice_seq_num;
        saw_any_frame = true;
    }

    if (header.slice_seq_num != current_slice_seq_num) {
        if (header.slice_seq_num != current_slice_seq_num + 1) {
            return format("slice_seq_num {} jumped from {}",
                          header.slice_seq_num, current_slice_seq_num);
        }
        if (had_previous_frame && !all_seen_channels_ended(*this)) {
            return format("slice transition before all channels reached END "
                          "at global_seq={}",
                          header.global_frame_seq_num);
        }
        current_slice_seq_num = header.slice_seq_num;
        next_channel_seq.fill(0);
        channel_seen.fill(false);
        channel_ended.fill(false);
        saw_non_metadata_in_slice = false;
    }

    std::size_t const index = channel_index(header.channel_type);
    if (channel_ended[index]) {
        return format("{} frame after channel END in slice {}",
                      channel_type_name(header.channel_type),
                      header.slice_seq_num);
    }
    if (header.channel_frame_seq_num != next_channel_seq[index]) {
        return format("channel_frame_seq_num {} != expected {} for {}",
                      header.channel_frame_seq_num, next_channel_seq[index],
                      channel_type_name(header.channel_type));
    }
    channel_seen[index] = true;
    next_channel_seq[index] = header.channel_frame_seq_num + 1;
    channel_ended[index] = has_frame_flag_end(header.flags);

    uint32_t const payload_capacity = block_size - fixed_header_size;
    if (!has_frame_flag_end(header.flags) &&
        header.frame_payload_size != payload_capacity) {
        return format("non-END {} payload must fill the record",
                      channel_type_name(header.channel_type));
    }

    if (header.channel_type == ChannelType::CH_METADATA) {
        if (saw_non_metadata_in_slice) {
            return "metadata frame after content in same slice";
        }
    } else {
        saw_non_metadata_in_slice = true;
    }

    remember_record(header, raw_data, record_size, skip_hash);
    return std::nullopt; // OK
}

RestoreFrameValidation
FrameValidator::validate_restore_frame(const FrameHeader &header,
                                       const uint8_t *raw_data,
                                       std::size_t record_size) {
    if (header.channel_type == ChannelType::CH_METADATA) {
        bool const hash_ok =
            verify_frame_hash(raw_data, record_size, header.frame_hash);
        if (auto err = validate(header, raw_data, record_size, !hash_ok);
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

RestoreFrameValidation
FrameValidator::validate_salvage_frame(const FrameHeader &header,
                                       const uint8_t *raw_data,
                                       std::size_t record_size) {
    last_was_replay = false;
    uint32_t const block_size = decoded_block_size(header);
    if (record_size != block_size) {
        return make_restore_validation(
            RestoreFrameValidationStatus::fatal,
            format("record size {} != decoded block size {}", record_size,
                   block_size));
    }
    if (!verify_frame_hash(raw_data, record_size, header.frame_hash)) {
        return make_restore_validation(
            RestoreFrameValidationStatus::fatal,
            format("frame hash mismatch at global_seq={}",
                   header.global_frame_seq_num));
    }
    bool const signature_present = std::ranges::any_of(
        header.signature, [](uint8_t byte) { return byte != 0; });
    if (has_frame_flag_signed(header.flags) != signature_present) {
        return make_restore_validation(
            RestoreFrameValidationStatus::fatal,
            "SIGNED flag and signature bytes are inconsistent");
    }
    if (archive_uuid != header.archive_uuid) {
        replay_history_.clear();
        replay_next_.reset();
        expected_global_frame_seq = 0;
        archive_uuid = header.archive_uuid;
    }
    if (auto error = check_replay(header, raw_data, record_size, false))
        return make_restore_validation(RestoreFrameValidationStatus::fatal,
                                       *error);
    if (!last_was_replay) {
        remember_record(header, raw_data, record_size, false);
        expected_global_frame_seq = header.global_frame_seq_num + 1;
    }
    return make_restore_validation(RestoreFrameValidationStatus::ok);
}

void FrameValidator::reset() {
    last_was_replay = false;
    replay_history_.clear();
    replay_next_.reset();
    archive_uuid.clear();
    archive_label.clear();
    expected_global_frame_seq = 0;
    current_slice_seq_num = 0;
    volume_block_size = 0;
    saw_any_frame = false;
    saw_archive_end = false;
    next_channel_seq.fill(0);
    channel_seen.fill(false);
    channel_ended.fill(false);
    saw_non_metadata_in_slice = false;
    stream_start_seeded = false;
}

} // namespace neotape
