#pragma once

#include "neotape/format.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace neotape {

enum class RestoreFrameValidationStatus { ok, warning, fatal };

struct RestoreFrameValidation {
    RestoreFrameValidationStatus status = RestoreFrameValidationStatus::ok;
    std::string message;
};

// Archive-level frame sequence validator.
//
// Feed frames in archival order via validate().  The validator tracks
// continuity invariants across the entire archive:
//
//   - global_frame_seq_num   monotonically increasing by 1
//   - volume_block_size      constant across all frames
//   - archive_uuid/label     constant across all frames
//   - logical_slice_seq_num  starts at 1, increments by at most 1
//   - channel ordering       metadata before content within a slice
//   - frame_seq_num_within_channel   contiguous per (slice, channel) group
//   - archive_end            must be the final frame, carries CLEAN_END
//
// Thread-compatible: single-threaded use only.
struct FrameValidator {
    // --- public state (read-only after feeding) ---
    std::string archive_uuid;
    std::string archive_label;
    uint64_t expected_global_frame_seq = 1;
    uint64_t expected_volume_seq_num = 0;
    uint64_t current_slice_seq_num = 0;
    uint32_t volume_block_size = 0; // decoded bytes
    ChannelType last_channel_type{};
    uint64_t expected_frame_seq_within_channel = 1;
    enum class Phase { none, metadata, content };
    Phase current_phase = Phase::none;
    bool saw_first_volume_seq = false;
    bool saw_any_frame = false;
    bool saw_archive_end = false;
    bool last_frame_had_end = false;

    // Validate one frame.  Returns error description or std::nullopt.
    //
    // header    — result of parse_fixed_header(raw_data, record_size)
    // raw_data  — pointer to the full record bytes (for hash check)
    // record_size — number of bytes in the record
    // skip_hash — when true, skip the frame_hash verification (use for
    //             advisory metadata frames where hash failure is non-fatal)
    //
    // After validate() returns with saw_archive_end == true, the
    // caller MUST stop sending frames.
    std::optional<std::string> validate(const FrameHeader &header,
                                        const uint8_t *raw_data,
                                        std::size_t record_size,
                                        bool skip_hash = false);

    // Validate one frame using restore-mode policy.
    //
    // Metadata frames are still checked for archive identity and sequencing,
    // but a metadata-only frame_hash mismatch is downgraded to a warning so a
    // payload reader can continue reconstructing ch_content.
    RestoreFrameValidation
    validate_restore_frame(const FrameHeader &header, const uint8_t *raw_data,
                           std::size_t record_size);

    // Reset to initial state (for inspecting a new archive).
    void reset();
};

} // namespace neotape
