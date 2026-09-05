#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// A record ends in NUL LF. Pathnames are bytes, including embedded LF and '/'.
class PlanReader {
  public:
    explicit PlanReader(const std::filesystem::path &path);
    std::optional<PlanRecord> next();

  private:
    std::ifstream input_;
    std::filesystem::path path_;
    uint64_t record_num_ = 0;
};
void write_plan_record(FILE *output, const PlanRecord &record);
} // namespace neotape
