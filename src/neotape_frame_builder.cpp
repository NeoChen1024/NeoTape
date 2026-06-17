#include "neotape/frame_builder.hpp"

#include <cassert>
#include <cstring>
#include <span>
#include <vector>

namespace neotape {

// ── Frame record helpers ──────────────────────────────────────────────

void copy_header_to_record(const HeaderBytes &header,
                           std::vector<std::byte> &record) {
    for (std::size_t i = 0; i < header.size(); ++i) {
        record[i] = static_cast<std::byte>(header[i]);
    }
}

void finalize_record_hash(FrameHeader &header, std::vector<std::byte> &record) {
    HeaderBytes header_bytes = serialize_frame_header(header);
    copy_header_to_record(header_bytes, record);
    header.frame_hash = compute_frame_hash(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
    header_bytes = serialize_frame_header(header);
    copy_header_to_record(header_bytes, record);
}

void patch_volume_seq_num(std::vector<std::byte> &record,
                          uint64_t new_volume_seq_num) {
    FrameHeader hdr = parse_fixed_header(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
    if (hdr.volume_seq_num == new_volume_seq_num) {
        return;
    }
    hdr.volume_seq_num = new_volume_seq_num;
    finalize_record_hash(hdr, record);
}

std::vector<std::byte> build_archive_end_record(uint32_t block_size,
                                                 uint64_t volume_seq_num,
                                                 const std::string &archive_uuid,
                                                 const std::string &archive_name,
                                                 uint64_t global_seq_num) {
    FrameHeader h;
    h.channel_type = ChannelType::ARCHIVE_END;
    h.volume_block_size_kib = static_cast<uint16_t>(block_size / 1024U);
    h.archive_uuid = archive_uuid;
    h.archive_label = archive_name;
    h.volume_seq_num = volume_seq_num;
    h.global_frame_seq_num = global_seq_num;
    h.logical_slice_seq_num = 0;
    h.frame_seq_num_within_channel = 1;
    h.frame_payload_size = 0;
    h.flags = frame_flag_start | frame_flag_end | frame_flag_clean_end;

    std::vector<std::byte> record(block_size, std::byte{0});
    finalize_record_hash(h, record);
    return record;
}

ContentFrameBuilder::ContentFrameBuilder(uint32_t block_size,
                                         std::string archive_uuid,
                                         std::string archive_name)
    : block_size_(block_size), archive_uuid_(std::move(archive_uuid)),
      archive_name_(std::move(archive_name)) {}

uint32_t ContentFrameBuilder::payload_capacity() const {
    return block_size_ - fixed_header_size;
}

void ContentFrameBuilder::set_current_slice(uint64_t slice_num) {
    current_slice_ = slice_num;
    frame_seq_within_channel_ = 1;
}

std::vector<BuiltFrame>
ContentFrameBuilder::feed(std::span<const std::byte> bytes) {
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());

    std::vector<BuiltFrame> out;
    const uint32_t cap = payload_capacity();
    while (pending_.size() > cap) {
        out.push_back(build_content_frame(
            std::span<const std::byte>(pending_.data(), cap), false));
        pending_.erase(pending_.begin(), pending_.begin() + cap);
    }
    return out;
}

std::optional<BuiltFrame> ContentFrameBuilder::flush() {
    if (pending_.empty()) {
        return std::nullopt;
    }
    BuiltFrame frame = build_content_frame(
        std::span<const std::byte>(pending_.data(), pending_.size()), true);
    pending_.clear();
    return frame;
}

BuiltFrame
ContentFrameBuilder::build_content_frame(std::span<const std::byte> payload,
                                         bool is_final) {
    assert(payload.size() <= payload_capacity());

    uint64_t flags = 0;
    if (frame_seq_within_channel_ == 1) {
        flags |= frame_flag_start;
    }
    if (is_final) {
        flags |= frame_flag_end;
    }

    FrameHeader fh;
    fh.channel_type = ChannelType::CH_CONTENT;
    fh.volume_block_size_kib = static_cast<uint16_t>(block_size_ / 1024U);
    fh.archive_uuid = archive_uuid_;
    fh.archive_label = archive_name_;
    fh.volume_seq_num = 0;
    fh.global_frame_seq_num = global_frame_seq_num_++;
    fh.logical_slice_seq_num = current_slice_;
    fh.frame_seq_num_within_channel = frame_seq_within_channel_++;
    fh.frame_payload_size = static_cast<uint32_t>(payload.size());
    fh.flags = flags;

    std::vector<std::byte> record(block_size_, std::byte{0});
    HeaderBytes const header = serialize_frame_header(fh);
    copy_header_to_record(header, record);
    std::copy(payload.begin(), payload.end(),
              record.begin() + static_cast<std::ptrdiff_t>(fixed_header_size));
    finalize_record_hash(fh, record);
    return BuiltFrame{std::move(record), fh.global_frame_seq_num};
}

// ── Frame retention buffer ───────────────────────────────────────────

FrameRetentionBuffer::FrameRetentionBuffer(size_t max_frames)
    : max_frames_(max_frames) {}

void FrameRetentionBuffer::add(uint64_t global_seq_num,
                                std::vector<std::byte> record) {
    if (frames_.size() == max_frames_) {
        frames_.pop_front();
    }
    frames_.push_back(RetainedFrame{global_seq_num, std::move(record)});
}

void FrameRetentionBuffer::ack(uint64_t global_seq_num) {
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
        if (it->global_seq_num == global_seq_num) {
            frames_.erase(it);
            return;
        }
    }
}

bool FrameRetentionBuffer::has(uint64_t global_seq_num) const {
    for (const auto &f : frames_) {
        if (f.global_seq_num == global_seq_num) {
            return true;
        }
    }
    return false;
}

const std::vector<std::byte> *
FrameRetentionBuffer::get(uint64_t global_seq_num) const {
    for (const auto &f : frames_) {
        if (f.global_seq_num == global_seq_num) {
            return &f.record;
        }
    }
    return nullptr;
}

uint64_t FrameRetentionBuffer::lowest_available() const {
    if (frames_.empty()) {
        return 0;
    }
    return frames_.front().global_seq_num;
}

bool FrameRetentionBuffer::empty() const { return frames_.empty(); }

} // namespace neotape
