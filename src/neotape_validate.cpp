#include "neotape/validate.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

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
    channel_seq_unknown.fill(true);
}

void FrameValidator::remember_record(const CheckedFrame &frame) {
    if (!frame.hash_ok)
        return;
    replay_history_.push_back({frame.header.global_frame_seq_num,
                               frame.header.volume_seq_num,
                               frame.header.frame_hash});
    constexpr std::size_t retry_history_records = 8192;
    if (replay_history_.size() > retry_history_records)
        replay_history_.pop_front();
}

void FrameValidator::begin_connection() { replay_next_.reset(); }

std::optional<string> FrameValidator::check_replay(const CheckedFrame &frame,
                                                   const uint8_t *data) {
    FrameHeader const &header = frame.header;
    if (header.global_frame_seq_num < expected_global_frame_seq) {
        if (!frame.hash_ok)
            return "replay failed frame integrity";
        auto const found =
            std::ranges::find_if(replay_history_, [&](const auto &entry) {
                return entry.global_seq == header.global_frame_seq_num;
            });
        if (found == replay_history_.end())
            return "cannot verify replay: prior comparison state unavailable";
        // Every byte except the volume ordinal, signature, and hash must
        // match: re-hash the retry as if written on the original volume.
        if (frame_hash_with_volume_seq(data, decoded_block_size(header),
                                       found->volume_seq) != found->frame_hash)
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

std::optional<string> FrameValidator::validate(const CheckedFrame &frame,
                                               const uint8_t *data,
                                               bool allow_bad_hash) {
    FrameHeader const &header = frame.header;
    last_was_replay = false;
    if (auto error = check_replay(frame, data))
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
    if (!frame.size_ok)
        return format("record size does not match decoded block size {}",
                      block_size);
    if (!frame.hash_ok && !allow_bad_hash)
        return format("frame hash mismatch at global_seq={}",
                      header.global_frame_seq_num);

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
        saw_archive_end = true;
        remember_record(frame);
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
        channel_seq_unknown.fill(false);
    }

    std::size_t const index = channel_index(header.channel_type);
    if (channel_ended[index]) {
        return format("{} frame after channel END in slice {}",
                      channel_type_name(header.channel_type),
                      header.slice_seq_num);
    }
    if (std::exchange(channel_seq_unknown[index], false))
        next_channel_seq[index] = header.channel_frame_seq_num;
    if (header.channel_frame_seq_num != next_channel_seq[index]) {
        return format("channel_frame_seq_num {} != expected {} for {}",
                      header.channel_frame_seq_num, next_channel_seq[index],
                      channel_type_name(header.channel_type));
    }
    channel_seen[index] = true;
    next_channel_seq[index] = header.channel_frame_seq_num + 1;
    channel_ended[index] = has_frame_flag_end(header.flags);

    remember_record(frame);
    return std::nullopt;
}

RestoreFrameValidation
FrameValidator::validate_restore_frame(const CheckedFrame &frame,
                                       const uint8_t *data) {
    // A metadata-only hash failure is advisory once the frame is otherwise
    // structurally placed in the stream.
    bool const advisory = frame.header.channel_type == ChannelType::CH_METADATA;
    if (auto err = validate(frame, data, advisory))
        return make_restore_validation(RestoreFrameValidationStatus::fatal,
                                       std::move(*err));
    if (!frame.hash_ok)
        return make_restore_validation(
            RestoreFrameValidationStatus::warning,
            format("metadata frame hash mismatch at global_seq={}",
                   frame.header.global_frame_seq_num));
    return make_restore_validation(RestoreFrameValidationStatus::ok);
}

RestoreFrameValidation
FrameValidator::validate_salvage_frame(const CheckedFrame &frame,
                                       const uint8_t *data) {
    FrameHeader const &header = frame.header;
    last_was_replay = false;
    if (!frame.size_ok || !frame.hash_ok)
        return make_restore_validation(
            RestoreFrameValidationStatus::fatal,
            format("frame {} at global_seq={}",
                   frame.size_ok ? "hash mismatch" : "size mismatch",
                   header.global_frame_seq_num));
    if (archive_uuid != header.archive_uuid) {
        replay_history_.clear();
        replay_next_.reset();
        expected_global_frame_seq = 0;
        archive_uuid = header.archive_uuid;
    }
    if (auto error = check_replay(frame, data))
        return make_restore_validation(RestoreFrameValidationStatus::fatal,
                                       *error);
    if (!last_was_replay) {
        remember_record(frame);
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
    channel_seq_unknown.fill(false);
    stream_start_seeded = false;
}

} // namespace neotape
