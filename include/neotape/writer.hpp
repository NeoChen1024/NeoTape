#pragma once

#include "neotape/signature.hpp"
#include "neotape/tape.hpp"
#include <optional>
#include <span>

namespace neotape {

// Media is positioned/authenticated by the CLI before entering the session.
// A null device discards records; capacity includes any prewritten bundle.
class RecordSink {
  public:
    RecordSink(mt::TapeDevice *device, std::optional<uint64_t> capacity = {},
               uint64_t used = 0);
    bool write(std::span<const std::byte> record); // true: written, now at EOT
    void filemark();

  private:
    mt::TapeDevice *device_;
    std::optional<uint64_t> capacity_;
    uint64_t used_;
};

enum class WriteStatus { complete, volume_full };
struct WriteResult {
    WriteStatus status;
    uint64_t frames;
    uint64_t final_global_seq;
};

// Throws on failure. One output thread owns every record write and ACK,
// including archive_end; the receiver owns validation and bounded enqueueing.
WriteResult write_volume(int socket, RecordSink &sink,
                         const std::vector<SignifyPublicKey> &keys,
                         size_t output_buffer_size);

} // namespace neotape
