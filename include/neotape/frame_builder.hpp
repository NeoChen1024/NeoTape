#pragma once

#include "neotape/format.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace neotape {

// ── Frame record helpers ──────────────────────────────────────────────

// Copy serialized header bytes into the first fixed_header_size bytes of
// a record vector.
void copy_header_to_record(const HeaderBytes &header,
                           std::vector<std::byte> &record);

// Serialize the header into the record, compute BLAKE3 frame hash over
// the full record, and patch the hash back into the header in-place.
void finalize_record_hash(FrameHeader &header,
                          std::vector<std::byte> &record);

// Update the volume_seq_num in a frame record and re-hash.
void patch_volume_seq_num(std::vector<std::byte> &record,
                          uint64_t new_volume_seq_num);

// Build a complete archive_end record.
std::vector<std::byte> build_archive_end_record(uint32_t block_size,
                                                 uint64_t volume_seq_num,
                                                 const std::string &archive_uuid,
                                                 const std::string &archive_name,
                                                 uint64_t global_seq_num);

// ── Frame retention buffer ───────────────────────────────────────────

// Bounded circular buffer that retains recently-sent frames so they can
// be re-sent if the client window allows retransmission (without waiting
// for the producer thread), or acknowledged and evicted.
struct RetainedFrame {
    uint64_t global_seq_num = 0;
    std::vector<std::byte> record;
};

class FrameRetentionBuffer {
  public:
    explicit FrameRetentionBuffer(size_t max_frames);

    void add(uint64_t global_seq_num, std::vector<std::byte> record);
    void ack(uint64_t global_seq_num);
    [[nodiscard]] const std::vector<std::byte> *
    get(uint64_t global_seq_num) const;

  private:
    size_t max_frames_;
    std::deque<RetainedFrame> frames_;
};


// A fully built NeoTape content frame.
struct BuiltFrame {
    std::vector<std::byte> record;
    uint64_t global_seq_num = 0;
};

// Frame builder for content-channel frames.
//
// Accumulates payload bytes and produces complete NeoTape records when a
// full frame is available. Correctly tracks:
//   - global_frame_seq_num (monotonic across the archive)
//   - frame_seq_num_within_channel (per (slice, channel) group)
//   - START flag on the first frame of a channel group
//   - END flag on the final frame (signalled by flush())
//
// A new logical slice resets frame_seq_num_within_channel to 1 without
// resetting the global frame counter.  Metadata-channel frames are not
// handled by this builder (the archiver is the metadata source, not the
// frame builder).
class ContentFrameBuilder {
  public:
    ContentFrameBuilder(uint32_t block_size, std::string archive_uuid,
                        std::string archive_name);

    [[nodiscard]] uint32_t payload_capacity() const;

    // Switch to a new logical slice.  Resets the within-channel sequence
    // number but does NOT flush pending bytes (caller should flush first).
    void set_current_slice(uint64_t slice_num);

    // Append payload bytes.  Returns zero or more complete frames.
    std::vector<BuiltFrame> feed(std::span<const std::byte> bytes);

    // Force any remaining pending bytes into a final frame.
    // The returned frame carries the END flag.
    std::optional<BuiltFrame> flush();

    // Access the next global seq num that will be assigned.
    [[nodiscard]] uint64_t next_global_seq_num() const {
        return global_frame_seq_num_;
    }

    // Last assigned global seq num (0 if none produced yet).
    [[nodiscard]] uint64_t last_global_seq_num() const {
        return global_frame_seq_num_ == 1 ? 0 : global_frame_seq_num_ - 1;
    }

    // Current logical slice sequence number.
    [[nodiscard]] uint64_t current_slice_seq_num() const {
        return current_slice_;
    }

  private:
    BuiltFrame build_content_frame(std::span<const std::byte> payload,
                                   bool is_final);

    uint32_t block_size_;
    std::string archive_uuid_;
    std::string archive_name_;
    uint64_t global_frame_seq_num_ = 1;
    uint64_t frame_seq_within_channel_ = 1;
    uint64_t current_slice_ = 1;
    std::vector<std::byte> pending_;
};

} // namespace neotape
