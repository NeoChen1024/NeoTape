#pragma once

#include "neotape/format.hpp"

#include <cstdint>
#include <string>

namespace neotape {

struct RestoreValidationState {
    std::string archive_uuid;
    std::string archive_name;
    PayloadProfile payload_profile = PayloadProfile::raw;
    uint64_t expected_volume_seq_num = 1;
    uint64_t current_volume_seq_num = 0;
    uint64_t expected_global_frame_seq_num = 1;
    uint64_t expected_logical_slice_seq_num = 1;
    uint64_t current_slice_size = 0;
    uint32_t volume_block_size = 0;
    bool identity_established = false;
    bool slice_open = false;
};

void accept_restore_volume_header(const VolumeHeader &vh,
                                  RestoreValidationState &state);
void validate_restore_frame_header(const FrameHeader &fh,
                                   const RestoreValidationState &state);
void note_restore_frame_accepted(const FrameHeader &fh,
                                 RestoreValidationState &state);
void validate_restore_archive_end(const ArchiveEndHeader &ae,
                                  const RestoreValidationState &state);

} // namespace neotape
