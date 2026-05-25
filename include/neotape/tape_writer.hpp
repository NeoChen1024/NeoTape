#pragma once

#include "neotape/cli.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace mt {

class TapeDevice;

using TapeChunkWriter = std::function<void(const uint8_t *, std::size_t, bool)>;
using TapePayloadProducer = std::function<void(TapeChunkWriter)>;

struct TapeWriterOptions {
    std::string device;
    std::string input = "-";
    std::string archive_name = "raw";
    uint32_t volume_block_size = 1024 * 1024;
    uint64_t slice_size = 0;
    bool slice_size_set = false;
    bool init_mode = false;
    bool init_if_blank = false;
    bool force_append = false;
    std::string payload_profile = "raw";
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
    std::function<void(bool)> status_pause;
};

void write_tape_archive(const TapeWriterOptions &opts);
void write_tape_archive_from_chunks_to_device(TapeDevice &dev,
                                              const TapeWriterOptions &opts,
                                              TapePayloadProducer producer);
void write_tape_archive_from_chunks(const TapeWriterOptions &opts,
                                    TapePayloadProducer producer);

} // namespace mt
