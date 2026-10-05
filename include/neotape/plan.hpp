#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <string>

namespace neotape {
struct PlannedEntry {
    uint64_t slice = 0, file_num = 0;
    char kind = '?';
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t uid = 0;
    std::string uname;
    uint32_t gid = 0;
    std::string gname, path;
};
struct PlanRecord {
    std::optional<std::string> chdir_dir;
    std::optional<PlannedEntry> entry;
};

// Outcome of reading one NUL LF terminated record.
enum class NulLfRecord {
    record,    // `text` holds the record without its terminator
    end,       // clean end of input
    truncated, // input ended inside a record or its terminator
    malformed, // NUL not followed by LF
};
// Shared record framing of plan and resume files. The caller checks the
// stream for I/O errors when the result is not `record`.
NulLfRecord read_nul_lf_record(std::istream &input, std::string &text);

// A record ends in NUL LF. Pathnames are bytes, including embedded LF and '/'.
class PlanReader {
  public:
    explicit PlanReader(const std::filesystem::path &path);
    // Read from a caller-owned stream; `name` labels it in error messages.
    PlanReader(std::istream &input, std::filesystem::path name);
    std::optional<PlanRecord> next();

  private:
    std::ifstream file_;
    std::istream &input_;
    std::filesystem::path path_;
    uint64_t record_num_ = 0;
    std::optional<PlannedEntry> previous_;
};
void write_plan_record(FILE *output, const PlanRecord &record);
} // namespace neotape
