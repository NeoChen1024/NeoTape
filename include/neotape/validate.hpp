#pragma once

#include "neotape/fec.hpp"
#include "neotape/format.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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
//   - slice_seq_num          starts at 0, increments by at most 1
//   - channel ordering       metadata before content within a slice
//   - channel_frame_seq_num  contiguous per (slice, channel) group
//   - archive_end            must be the final frame, carries CLEAN_END
//
// Thread-compatible: single-threaded use only.
struct FrameValidator {
    // --- public state (read-only after feeding) ---
    std::string archive_uuid;
    std::string archive_label;
    uint64_t expected_global_frame_seq = 0;
    uint64_t expected_volume_seq_num = 0;
    uint64_t current_slice_seq_num = 0;
    uint32_t volume_block_size = 0; // decoded bytes
    ChannelType last_channel_type{};
    uint64_t expected_channel_frame_seq_num = 0;
    enum class Phase { none, metadata, content };
    Phase current_phase = Phase::none;
    bool saw_first_volume_seq = false;
    bool saw_any_frame = false;
    bool saw_archive_end = false;
    bool last_frame_had_end = false;

    // Per-channel state is required because ch_content and ch_fec may be
    // physically interleaved while retaining independent sequence streams.
    std::array<uint64_t, 3> next_channel_seq{};
    std::array<bool, 3> channel_seen{};
    std::array<bool, 3> channel_ended{};
    bool saw_non_metadata_in_slice = false;
    bool archive_uses_fec = false;
    bool stream_start_seeded = false;
    bool validating_seed_frame = false;

    uint64_t protected_run_start = 0;
    uint16_t protected_run_count = 0;
    bool protected_run_started_before_stream = false;
    uint64_t protected_run_size = 0;
    std::vector<uint8_t> protected_run_bytes;
    std::optional<FecDescriptor> current_fec_group;
    uint16_t next_repair_index = 0;

    // Seed connection-local validation when reading begins at a volume
    // boundary rather than archive-global frame zero. The supplied header is
    // still validated normally by the next validate() call.
    void seed_for_stream_start(const FrameHeader &header);

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
    RestoreFrameValidation validate_restore_frame(const FrameHeader &header,
                                                  const uint8_t *raw_data,
                                                  std::size_t record_size);

    // Salvage mode keeps frame integrity and unambiguous record framing
    // mandatory, but deliberately does not enforce archive identity,
    // sequencing, channel ordering, or clean-completion consistency.
    RestoreFrameValidation
    validate_salvage_frame(const FrameHeader &header, const uint8_t *raw_data,
                           std::size_t record_size) const;

    // Reset to initial state (for inspecting a new archive).
    void reset();
};

} // namespace neotape
