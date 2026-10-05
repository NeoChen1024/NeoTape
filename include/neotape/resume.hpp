#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace neotape {

// Archive identity a resumed archiver run must reproduce or match.
struct ResumeHeader {
    std::string archive_uuid;
    uint32_t block_size_kib = 0;
    std::string sign_key_id; // lowercase hex; empty when frames are unsigned
    std::string plan_blake3; // lowercase hex; empty when unknown
    std::string archive_name;
};

// Start of a slice: its first frame's global sequence number and the volume
// that frame was committed to.
struct ResumePoint {
    uint64_t slice = 0;
    uint64_t global_seq = 0;
    uint64_t volume = 0;
};

struct ResumeState {
    ResumeHeader header;
    // Effective slice starts in increasing slice order. A later record for
    // slice N replaces the earlier records for N and every slice after it.
    std::vector<ResumePoint> points;
    // Size of the leading run of complete records.
    uint64_t valid_size = 0;
};

// Serialized records, each terminated by NUL LF like plan records.
std::string encode_resume_header(const ResumeHeader &header);
std::string encode_resume_point(const ResumePoint &point);

ResumeState read_resume_file(const std::filesystem::path &path);

std::string blake3_file_hex(const std::filesystem::path &path);

// Appends slice starts to a resume file as the writer acknowledges them.
// The producer thread announces slice starts; the thread handling ACKs
// commits them. A failed append disables the log with a warning rather
// than failing the archive.
class ResumeLog {
  public:
    // Create a new resume file; fails if `path` already exists.
    ResumeLog(const std::filesystem::path &path, const ResumeHeader &header);
    // Continue `path` after `state`. `restart` is the slice start the run
    // resumes from, if any; it is recorded again first.
    ResumeLog(const std::filesystem::path &path, const ResumeState &state,
              const std::optional<ResumePoint> &restart);
    ~ResumeLog();

    ResumeLog(const ResumeLog &) = delete;
    ResumeLog &operator=(const ResumeLog &) = delete;

    void slice_started(uint64_t slice, uint64_t global_seq);
    void frame_acked(uint64_t global_seq, uint64_t volume);

  private:
    void append(const std::string &records);

    std::filesystem::path path_;
    int fd_ = -1;
    std::mutex mutex_;
    struct PendingSlice {
        uint64_t slice = 0, global_seq = 0;
    };
    std::deque<PendingSlice> pending_;
    std::optional<uint64_t> last_recorded_slice_;
};

} // namespace neotape
