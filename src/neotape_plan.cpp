#include "neotape/plan.hpp"
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace neotape {
namespace {
namespace fs = std::filesystem;
using std::format;
using std::string;
using std::string_view;
using std::vector;
uint64_t parse_u64_field(const string &field, const fs::path &path,
                         uint64_t record_num) {
    char *end = nullptr;
    unsigned long long const value = std::strtoull(field.c_str(), &end, 10);
    if (end == field.c_str() || *end != '\0') {
        throw std::runtime_error(
            format("{}:{}: invalid numeric field", path.string(), record_num));
    }
    return static_cast<uint64_t>(value);
}

PlanRecord parse_plan_record(string_view text, const fs::path &path,
                             uint64_t record_num) {
    if (text.starts_with("/chdir/")) {
        return PlanRecord{
            .chdir_dir = string(text.substr(7)),
            .entry = std::nullopt,
        };
    }
    if (text.empty() || text.front() != '/') {
        throw std::runtime_error(
            format("{}:{}: invalid plan record", path.string(), record_num));
    }

    // 9 slash-delimited fields: slice/file_num/kind/size/mtime/uid/
    // uname/gid/gname — then the remainder is the path.
    vector<string> fields;
    size_t start = 1;
    for (size_t i = 1; i <= text.size() && fields.size() < 9; ++i) {
        if (i == text.size() || text[i] == '/') {
            fields.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (fields.size() != 9 || start > text.size()) {
        throw std::runtime_error(
            format("{}:{}: invalid entry record", path.string(), record_num));
    }

    string entry_path(text.substr(start));
    if (entry_path.empty() || fields[2].size() != 1) {
        throw std::runtime_error(
            format("{}:{}: invalid entry record", path.string(), record_num));
    }

    char const kind = fields[2][0];

    return PlanRecord{
        .chdir_dir = std::nullopt,
        .entry =
            PlannedEntry{
                .slice = parse_u64_field(fields[0], path, record_num),
                .file_num = parse_u64_field(fields[1], path, record_num),
                .kind = kind,
                .size = parse_u64_field(fields[3], path, record_num),
                .mtime = static_cast<int64_t>(
                    parse_u64_field(fields[4], path, record_num)),
                .uid = static_cast<uint32_t>(
                    parse_u64_field(fields[5], path, record_num)),
                .uname = fields[6],
                .gid = static_cast<uint32_t>(
                    parse_u64_field(fields[7], path, record_num)),
                .gname = fields[8],
                .path = std::move(entry_path),
            },
    };
}

} // namespace

PlanReader::PlanReader(const std::filesystem::path &path)
    : input_(path, std::ios::binary), path_(path) {
    if (!input_)
        throw std::runtime_error("open plan: " + path.string());
}

std::optional<PlanRecord> PlanReader::next() {
    std::string record;
    if (!std::getline(input_, record, '\0')) {
        if (input_.eof() && record.empty())
            return std::nullopt;
        throw std::runtime_error("read plan: " + path_.string());
    }
    ++record_num_;
    if (input_.eof() || input_.get() != '\n')
        throw std::runtime_error(std::format("{}:{}: unterminated plan record",
                                             path_.string(), record_num_));
    return parse_plan_record(record, path_, record_num_);
}

void write_plan_record(FILE *output, const PlanRecord &record) {
    std::string text;
    if (record.chdir_dir)
        text = "/chdir/" + *record.chdir_dir;
    else if (record.entry) {
        const auto &e = *record.entry;
        text = std::format("/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}", e.slice,
                           e.file_num, e.kind, e.size, e.mtime, e.uid, e.uname,
                           e.gid, e.gname, e.path);
    } else
        throw std::invalid_argument("empty plan record");
    text.append("\0\n", 2);
    if (std::fwrite(text.data(), 1, text.size(), output) != text.size())
        throw std::runtime_error("write plan record failed");
}
} // namespace neotape
