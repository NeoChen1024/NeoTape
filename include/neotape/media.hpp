#pragma once

#include "neotape/format.hpp"
#include "neotape/tape.hpp"
#include <memory>
#include <optional>

namespace neotape {
struct MediaLocator {
    enum Kind { none, tape, spool, null_sink } kind = none;
    std::string path;
};
MediaLocator parse_media(std::string_view text, bool allow_null = false);

// The file number of a spool tape file name `NT<1-6 digits>.NTS` in any
// letter case, or nullopt for any other name.
std::optional<uint64_t> spool_file_number(const std::filesystem::path &path);
// Spool tape files in file-number order. Throws when the directory is
// missing or two files share a number.
std::vector<std::filesystem::path>
scan_spool_files(const std::filesystem::path &root);
// Parse the fixed header of the first record in one spool file.
FrameHeader read_spool_file_header(const std::filesystem::path &path);

using mt::MediaRecord;
using mt::RecordEvent;

// Records of a tape or spool source in media order. Hash, signature and
// archive validation belong to callers.
// A tape source is read from its current position: an archive need not start
// at BOT, so positioning is left to the operator. Spool sources start at their
// first file.
class RecordReader {
  public:
    explicit RecordReader(const MediaLocator &source);
    MediaRecord next();
    void skip_file(); // first-record-only scan; never drain the file to skip it
  private:
    std::unique_ptr<mt::TapeDevice> device_;
    bool ended_ = false;
};
} // namespace neotape
