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
    case ChannelType::CH_FEC:
        return 2;
    case ChannelType::ARCHIVE_END:
        break;
    }
    throw std::runtime_error("archive_end has no slice channel index");
}

bool same_fec_group(const FecDescriptor &left, const FecDescriptor &right) {
    return left.fec_version == right.fec_version &&
           left.fec_profile == right.fec_profile &&
           left.fec_flags == right.fec_flags &&
           left.source_content_frame_start ==
               right.source_content_frame_start &&
           left.source_frame_count == right.source_frame_count &&
           left.source_stream_size == right.source_stream_size &&
           left.fec_group_blake3 == right.fec_group_blake3;
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
    last_channel_type = header.channel_type;
    current_phase =
        header.channel_type == ChannelType::CH_CONTENT    ? Phase::content
        : header.channel_type == ChannelType::CH_METADATA ? Phase::metadata
                                                          : Phase::none;
    stream_start_seeded = true;
    validating_seed_frame = true;
    if (header.channel_type != ChannelType::ARCHIVE_END) {
        std::size_t const index = channel_index(header.channel_type);
        next_channel_seq[index] = header.channel_frame_seq_num;
    }
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

    bool const had_previous_frame = saw_any_frame;
    bool const seeded_frame = validating_seed_frame;

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
    validating_seed_frame = false;

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
        if (had_previous_frame &&
            (!all_seen_channels_ended(*this) || protected_run_count != 0 ||
             current_fec_group.has_value())) {
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
        if (had_previous_frame &&
            (!all_seen_channels_ended(*this) || protected_run_count != 0 ||
             current_fec_group.has_value())) {
            return format("slice transition before all channels reached END "
                          "at global_seq={}",
                          header.global_frame_seq_num);
        }
        current_slice_seq_num = header.slice_seq_num;
        current_phase = Phase::none;
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
    if (header.channel_type == ChannelType::CH_FEC) {
        if (header.frame_payload_size != payload_capacity) {
            return "ch_fec payload must fill the record";
        }
    } else if (!has_frame_flag_end(header.flags) &&
               header.frame_payload_size != payload_capacity) {
        return format("non-END {} payload must fill the record",
                      channel_type_name(header.channel_type));
    }

    if (header.channel_type == ChannelType::CH_METADATA) {
        if (saw_non_metadata_in_slice) {
            return "metadata frame after content or FEC in same slice";
        }
        current_phase = Phase::metadata;
    } else {
        saw_non_metadata_in_slice = true;
        current_phase = Phase::content;
    }

    if (header.channel_type == ChannelType::CH_CONTENT) {
        if (current_fec_group.has_value()) {
            return "content frame before preceding FEC group completed";
        }
        if (has_frame_flag_fec_protected(header.flags)) {
            archive_uses_fec = true;
            if (protected_run_count == 0) {
                protected_run_start = header.channel_frame_seq_num;
                protected_run_started_before_stream =
                    seeded_frame && header.channel_frame_seq_num != 0;
                protected_run_bytes.clear();
                protected_run_size = 0;
                protected_run_has_unavailable = false;
            }
            if (protected_run_count == fec_data_shards) {
                return "FEC_PROTECTED run exceeds 32 content frames";
            }
            ++protected_run_count;
            protected_run_size += header.frame_payload_size;
            if (skip_hash) {
                protected_run_has_unavailable = true;
            } else {
                protected_run_bytes.insert(
                    protected_run_bytes.end(), raw_data + fixed_header_size,
                    raw_data + fixed_header_size + header.frame_payload_size);
            }
        } else {
            if (protected_run_count != 0) {
                return "unprotected content before matching FEC group";
            }
            if (archive_uses_fec) {
                return "content became unprotected after archive enabled FEC";
            }
        }
    } else if (header.channel_type == ChannelType::CH_FEC) {
        FecDescriptor descriptor;
        try {
            descriptor = parse_fec_descriptor(header.sideband_data);
            validate_fec_descriptor(descriptor, payload_capacity);
        } catch (const std::exception &error) {
            return format("invalid FEC descriptor: {}", error.what());
        }

        if (seeded_frame && protected_run_count == 0) {
            current_fec_group = descriptor;
            next_repair_index = descriptor.repair_index;
        } else if (!current_fec_group.has_value()) {
            if (protected_run_count == 0) {
                return "ch_fec without preceding FEC_PROTECTED content run";
            }
            if (protected_run_started_before_stream) {
                uint64_t const observed_end =
                    protected_run_start + protected_run_count;
                uint64_t const described_end =
                    descriptor.source_content_frame_start +
                    descriptor.source_frame_count;
                if (observed_end != described_end ||
                    descriptor.source_content_frame_start >=
                        protected_run_start) {
                    return "FEC descriptor does not cover seeded protected run";
                }
            } else {
                if (descriptor.source_content_frame_start !=
                        protected_run_start ||
                    descriptor.source_frame_count != protected_run_count ||
                    descriptor.source_stream_size != protected_run_size) {
                    return "FEC descriptor does not match protected content "
                           "run";
                }
                if (!protected_run_has_unavailable) {
                    Hash const group_hash = blake3_hash(
                        protected_run_bytes.data(), protected_run_bytes.size());
                    if (descriptor.fec_group_blake3 != group_hash) {
                        return "FEC group hash does not match protected "
                               "content";
                    }
                }
            }
            current_fec_group = descriptor;
            next_repair_index = 0;
        } else if (!same_fec_group(*current_fec_group, descriptor)) {
            return "FEC descriptors disagree within repair group";
        }

        if (descriptor.repair_index != next_repair_index) {
            return format("FEC repair_index {} != expected {}",
                          descriptor.repair_index, next_repair_index);
        }
        ++next_repair_index;
        if (next_repair_index == fec_repair_shards) {
            current_fec_group.reset();
            next_repair_index = 0;
            protected_run_count = 0;
            protected_run_started_before_stream = false;
            protected_run_size = 0;
            protected_run_has_unavailable = false;
            protected_run_bytes.clear();
        }
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

RestoreFrameValidation
FrameValidator::validate_salvage_frame(const FrameHeader &header,
                                       const uint8_t *raw_data,
                                       std::size_t record_size) const {
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
    if (header.channel_type == ChannelType::CH_FEC) {
        try {
            FecDescriptor const descriptor =
                parse_fec_descriptor(header.sideband_data);
            validate_fec_descriptor(descriptor, block_size - fixed_header_size);
        } catch (const std::exception &error) {
            return make_restore_validation(
                RestoreFrameValidationStatus::fatal,
                format("invalid FEC descriptor: {}", error.what()));
        }
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
    next_channel_seq.fill(0);
    channel_seen.fill(false);
    channel_ended.fill(false);
    saw_non_metadata_in_slice = false;
    archive_uses_fec = false;
    stream_start_seeded = false;
    validating_seed_frame = false;
    protected_run_start = 0;
    protected_run_count = 0;
    protected_run_started_before_stream = false;
    protected_run_size = 0;
    protected_run_has_unavailable = false;
    protected_run_bytes.clear();
    current_fec_group.reset();
    next_repair_index = 0;
}

} // namespace neotape
