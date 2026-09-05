#pragma once

#include "neotape/tape.hpp"
#include <memory>

namespace neotape {
struct MediaLocator {
    enum Kind { none, tape, spool, null_sink } kind = none;
    std::string path;
};
MediaLocator parse_media(std::string_view text, bool allow_null = false);
bool parse_spool_file_name(const std::filesystem::path &path, uint64_t &number);
std::vector<std::filesystem::path>
scan_spool_files(const std::filesystem::path &root);

enum class RecordEvent { record, filemark, end };
struct MediaRecord {
    RecordEvent event = RecordEvent::end;
    std::vector<std::byte> record;
    uint64_t file_num = 0;
    std::string source_name;
};

// Physical tape records are opaque. Spool streams need only header parsing to
// locate record boundaries; hash/signature/archive validation belongs to
// callers.
class RecordReader {
  public:
    explicit RecordReader(const MediaLocator &source);
    ~RecordReader();
    RecordReader(const RecordReader &) = delete;
    RecordReader &operator=(const RecordReader &) = delete;
    MediaRecord next();
    void skip_file(); // first-record-only scan; never drain the file to skip it
  private:
    void open_spool();
    std::unique_ptr<mt::TapeDevice> tape_;
    std::vector<std::filesystem::path> files_;
    std::vector<std::byte> buffer_;
    size_t index_ = 0;
    uint64_t file_num_ = 0;
    int spool_fd_ = -1;
    bool ended_ = false;
};
} // namespace neotape
