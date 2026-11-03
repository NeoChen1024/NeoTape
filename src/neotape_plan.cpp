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
int64_t parse_mtime(const string &field) {
    int64_t value = 0;
    auto [end, error] =
        std::from_chars(field.data(), field.data() + field.size(), value);
    if (error != std::errc{} || end != field.data() + field.size())
        throw std::runtime_error("invalid mtime");
    return value;
}

// Errors name the offending field; the caller prefixes the record location.
PlanRecord parse_plan_record(string_view text) {
    if (text.starts_with("/chdir/")) {
        return PlanRecord{
            .chdir_dir = string(text.substr(7)),
            .entry = std::nullopt,
        };
    }
    if (text.empty() || text.front() != '/') {
        throw std::runtime_error("invalid plan record");
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
        throw std::runtime_error("invalid entry record");
    }

    string entry_path(text.substr(start));
    if (entry_path.empty() || fields[2].size() != 1) {
        throw std::runtime_error("invalid entry record");
    }

    char const kind = fields[2][0];
    if (string_view("fhdlcbps").find(kind) == string_view::npos ||
        entry_path.front() == '/')
        throw std::runtime_error("invalid entry kind or path");
    uint64_t const size = parse_uint(fields[3], "size");
    if (kind == 'h' && size != 0)
        throw std::runtime_error("hardlink entry size must be zero");
    constexpr uint64_t max_id = std::numeric_limits<uint32_t>::max();

    return PlanRecord{
        .chdir_dir = std::nullopt,
        .entry =
            PlannedEntry{
                .slice = parse_uint(fields[0], "slice"),
                .file_num = parse_uint(fields[1], "file number"),
                .kind = kind,
                .size = size,
                .mtime = parse_mtime(fields[4]),
                .uid = static_cast<uint32_t>(
                    parse_uint(fields[5], "owner ID", 0, max_id)),
                .uname = fields[6],
                .gid = static_cast<uint32_t>(
                    parse_uint(fields[7], "group ID", 0, max_id)),
                .gname = fields[8],
                .path = std::move(entry_path),
            },
    };
}

} // namespace

NulLfRecord read_nul_lf_record(std::istream &input, std::string &text) {
    if (!std::getline(input, text, '\0'))
        return text.empty() && input.eof() ? NulLfRecord::end
                                           : NulLfRecord::truncated;
    if (input.eof())
        return NulLfRecord::truncated;
    int const next = input.get();
    if (next == std::char_traits<char>::eof())
        return NulLfRecord::truncated;
    return next == '\n' ? NulLfRecord::record : NulLfRecord::malformed;
}

PlanReader::PlanReader(const std::filesystem::path &path)
    : file_(path, std::ios::binary), input_(file_), path_(path) {
    if (!file_)
        throw std::runtime_error("open plan: " + path.string());
}

PlanReader::PlanReader(std::istream &input, std::filesystem::path name)
    : input_(input), path_(std::move(name)) {}

std::optional<PlanRecord> PlanReader::next() {
    std::string record;
    NulLfRecord const framing = read_nul_lf_record(input_, record);
    if (input_.bad())
        throw std::runtime_error("read plan: " + path_.string());
    if (framing == NulLfRecord::end)
        return std::nullopt;
    ++record_num_;
    try {
        if (framing != NulLfRecord::record)
            throw std::runtime_error("unterminated plan record");
        auto parsed = parse_plan_record(record);
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
    } catch (const std::exception &error) {
        throw std::runtime_error(std::format("{}:{}: {}", path_.string(),
                                             record_num_, error.what()));
    }
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
