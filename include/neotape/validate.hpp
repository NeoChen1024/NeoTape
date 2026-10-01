#pragma once

#include "neotape/format.hpp"

#include <array>
#include <cstddef>
#include <deque>
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
//   - slice_seq_num          starts at 0, increments by at most 1
//   - channel ordering       metadata before content within a slice
//   - channel_frame_seq_num  contiguous per (slice, channel) group
//   - archive_end            must be the final frame, carries CLEAN_END
//
// Thread-compatible: single-threaded use only.
struct FrameValidator {
    bool last_was_replay = false;
    void begin_connection();

    // --- public state (read-only after feeding) ---
    std::string archive_uuid;
    std::string archive_label;
    uint64_t expected_global_frame_seq = 0;
    uint64_t current_slice_seq_num = 0;
    uint32_t volume_block_size = 0; // decoded bytes
    bool saw_any_frame = false;
    bool saw_archive_end = false;

    // Each slice channel keeps an independent sequence stream and END state.
    std::array<uint64_t, 2> next_channel_seq{};
    std::array<bool, 2> channel_seen{};
    std::array<bool, 2> channel_ended{};
    bool saw_non_metadata_in_slice = false;
    bool stream_start_seeded = false;

    // Seed connection-local validation when reading begins at a volume
    // boundary rather than archive-global frame zero. The supplied header is
    // still validated normally by the next validate() call.
    void seed_for_stream_start(const FrameHeader &header);

    // Validate one frame.  Returns error description or std::nullopt.
    //
    // header    — result of parse_fixed_header(raw_data, record_size)
    // raw_data  — pointer to the full record bytes (for hash check)
    // record_size — number of bytes in the record
    // skip_hash — when true, skip frame_hash verification while retaining
    //             structural/state validation. Used for advisory metadata
    //             whose hash failure restore mode downgrades to a warning.
    //
    // last_was_replay identifies an equivalent physical retry; callers must
    // still apply signature policy, then suppress repeated output. After
    // archive_end, only equivalent replays are accepted within this context.
    std::optional<std::string> validate(const FrameHeader &header,
                                        const uint8_t *raw_data,
                                        std::size_t record_size,
                                        bool skip_hash = false);

    // Validate one frame using restore-mode policy.
    //
    // Metadata frames are still checked for archive identity and sequencing,
    // but a metadata-only frame_hash mismatch is downgraded to a warning.
    // Callers enforce signature policy before using that exception.
    RestoreFrameValidation validate_restore_frame(const FrameHeader &header,
                                                  const uint8_t *raw_data,
                                                  std::size_t record_size);

    // Salvage mode keeps frame integrity and unambiguous record framing
    // mandatory, but deliberately does not enforce archive identity,
    // sequencing, channel ordering, or clean-completion consistency.
    RestoreFrameValidation validate_salvage_frame(const FrameHeader &header,
                                                  const uint8_t *raw_data,
                                                  std::size_t record_size);

    // Reset to initial state (for inspecting a new archive).
    void reset();

  private:
    // Keep retry memory bounded even for multi-day archives. Older retries
    // fail explicitly instead of being silently treated as equivalent.
    std::deque<std::pair<uint64_t, Hash>> replay_history_;
    std::optional<uint64_t> replay_next_;
    std::optional<std::string> check_replay(const FrameHeader &header,
                                            const uint8_t *data,
                                            std::size_t size, bool skip_hash);
    void remember_record(const FrameHeader &header, const uint8_t *data,
                         std::size_t size, bool skip_hash);
};

} // namespace neotape
