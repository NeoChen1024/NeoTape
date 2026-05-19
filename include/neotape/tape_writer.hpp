#pragma once

#include <cstdint>
#include <string>

namespace mt {

struct TapeWriterOptions {
    std::string device;
    std::string input = "-";
    std::string archive_name = "raw";
    uint32_t volume_block_size = 1024 * 1024;
    uint64_t slice_size = 64ull * 1024 * 1024;
    bool init_mode = false;
    bool init_if_blank = false;
    bool force_append = false;
};

void write_tape_archive(const TapeWriterOptions &opts);

} // namespace mt
