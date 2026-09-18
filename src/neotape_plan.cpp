#include "neotape/common.hpp"
#include "neotape/plan.hpp"
#include <charconv>
#include <format>
#include <limits>
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
    return parse_uint(field,
                      format("{}:{} numeric field", path.string(), record_num));
}

int64_t parse_mtime(const string &field) {
    int64_t value = 0;
    auto [end, error] =
        std::from_chars(field.data(), field.data() + field.size(), value);
    if (error != std::errc{} || end != field.data() + field.size())
        throw std::runtime_error("invalid signed plan mtime");
    return value;
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
    if (string_view("fhdlcbps").find(kind) == string_view::npos ||
        entry_path.front() == '/')
        throw std::runtime_error("invalid plan entry kind or path");
    if (kind == 'h' && parse_u64_field(fields[3], path, record_num) != 0)
        throw std::runtime_error("hardlink plan entry size must be zero");

    return PlanRecord{
        .chdir_dir = std::nullopt,
        .entry =
            PlannedEntry{
                .slice = parse_u64_field(fields[0], path, record_num),
                .file_num = parse_u64_field(fields[1], path, record_num),
                .kind = kind,
                .size = parse_u64_field(fields[3], path, record_num),
                .mtime = parse_mtime(fields[4]),
                .uid = static_cast<uint32_t>(
                    parse_uint(fields[5], "plan owner ID", 0,
                               std::numeric_limits<uint32_t>::max())),
                .uname = fields[6],
                .gid = static_cast<uint32_t>(
                    parse_uint(fields[7], "plan owner ID", 0,
                               std::numeric_limits<uint32_t>::max())),
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
    auto parsed = parse_plan_record(record, path_, record_num_);
    if (parsed.chdir_dir) {
        if (record_num_ != 1 || parsed.chdir_dir->empty())
            throw std::runtime_error(
                "chdir must be the first non-empty plan directive");
    } else {
        auto const &entry = *parsed.entry;
        bool valid = !previous_ ? entry.slice == 0 && entry.file_num == 0
                     : entry.slice == previous_->slice
                         ? previous_->file_num != UINT64_MAX &&
                               entry.file_num == previous_->file_num + 1
                         : previous_->slice != UINT64_MAX &&
                               entry.slice == previous_->slice + 1 &&
                               entry.file_num == 0;
        if (!valid)
            throw std::runtime_error(
                "non-contiguous plan slice or file number");
        previous_ = entry;
    }
    return parsed;
}

void write_plan_record(FILE *output, const PlanRecord &record) {
    std::string text;
    if (record.chdir_dir)
        text = "/chdir/" + *record.chdir_dir;
    else if (record.entry) {
        const auto &e = *record.entry;
        auto name = [](const string &value) -> string {
            return value.find_first_of(string("/\0", 2)) == string::npos
                       ? value
                       : string{};
        };
        text = std::format("/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}", e.slice,
                           e.file_num, e.kind, e.size, e.mtime, e.uid,
                           name(e.uname), e.gid, name(e.gname), e.path);
    } else
        throw std::invalid_argument("empty plan record");
    text.append("\0\n", 2);
    if (std::fwrite(text.data(), 1, text.size(), output) != text.size())
        throw std::runtime_error("write plan record failed");
}
} // namespace neotape
