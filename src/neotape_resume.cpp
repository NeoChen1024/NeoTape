#include "neotape/resume.hpp"

#include "neotape/common.hpp"
#include "neotape/plan.hpp"

#include <blake3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace neotape {
namespace {
namespace fs = std::filesystem;
using std::format;
using std::string;
using std::string_view;

constexpr string_view format_record = "/neotape-resume/1";

bool is_lower_hex(string_view text, size_t size) {
    return text.size() == size &&
           text.find_first_not_of("0123456789abcdef") == string_view::npos;
}

[[noreturn]] void throw_errno(const fs::path &path, string_view action) {
    int const saved = errno;
    throw std::runtime_error(format("{} resume file {}: {}", action,
                                    path.string(), std::strerror(saved)));
}

ResumePoint parse_point(string_view value, const string &where) {
    size_t const first = value.find('/');
    size_t const second =
        first == string_view::npos ? first : value.find('/', first + 1);
    if (second == string_view::npos)
        throw std::runtime_error(where + ": invalid slice record");
    return ResumePoint{
        .slice = parse_uint(value.substr(0, first), where + " slice", 1),
        .global_seq = parse_uint(value.substr(first + 1, second - first - 1),
                                 where + " global sequence number"),
        .volume = parse_uint(value.substr(second + 1), where + " volume"),
    };
}

void add_point(std::vector<ResumePoint> &points, const ResumePoint &point,
               const string &where) {
    if (!points.empty() && point.slice > points.back().slice + 1)
        throw std::runtime_error(where + ": slice number skips a slice");
    while (!points.empty() && points.back().slice >= point.slice)
        points.pop_back();
    if (!points.empty() && (point.global_seq <= points.back().global_seq ||
                            point.volume < points.back().volume))
        throw std::runtime_error(
            where + ": slice start does not follow the previous slice");
    points.push_back(point);
}

} // namespace

string encode_resume_header(const ResumeHeader &header) {
    string text;
    auto add = [&](string_view key, string_view value) {
        text += format("/{}/{}", key, value);
        text.append("\0\n", 2);
    };
    text += format_record;
    text.append("\0\n", 2);
    add("archive_uuid", header.archive_uuid);
    add("block_size_kib", std::to_string(header.block_size_kib));
    add("sign_key_id", header.sign_key_id);
    add("plan_blake3", header.plan_blake3);
    add("archive_name", header.archive_name);
    return text;
}

string encode_resume_point(const ResumePoint &point) {
    string text =
        format("/slice/{}/{}/{}", point.slice, point.global_seq, point.volume);
    text.append("\0\n", 2);
    return text;
}

ResumeState read_resume_file(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("open resume file: " + path.string());

    ResumeState state;
    // One flag per header record, in encode_resume_header order.
    std::array<bool, 5> seen{};
    uint64_t record_num = 0;
    string record;
    for (;;) {
        NulLfRecord const framing = read_nul_lf_record(input, record);
        if (input.bad())
            throw std::runtime_error("read resume file: " + path.string());
        // A record cut short by a crash can only be the last one.
        if (framing == NulLfRecord::end || framing == NulLfRecord::truncated)
            break;
        ++record_num;
        string const where = format("{}:{}", path.string(), record_num);
        if (framing == NulLfRecord::malformed)
            throw std::runtime_error(where + ": invalid record terminator");
        if (record_num == 1) {
            if (record != format_record)
                throw std::runtime_error(
                    where + ": not a NeoTape resume file of version 1");
            state.valid_size += record.size() + 2;
            continue;
        }

        size_t const split = record.find('/', 1);
        if (record.empty() || record.front() != '/' || split == string::npos)
            throw std::runtime_error(where + ": invalid record");
        string_view const key = string_view(record).substr(1, split - 1);
        string_view const value = string_view(record).substr(split + 1);

        if (key == "slice") {
            if (std::ranges::find(seen, false) != seen.end())
                throw std::runtime_error(
                    where + ": slice record before the complete header");
            add_point(state.points, parse_point(value, where), where);
            state.valid_size += record.size() + 2;
            continue;
        }

        size_t index = 0;
        if (key == "archive_uuid") {
            index = 0;
            if (value.empty())
                throw std::runtime_error(where + ": empty archive UUID");
            state.header.archive_uuid = value;
        } else if (key == "block_size_kib") {
            index = 1;
            state.header.block_size_kib = static_cast<uint32_t>(
                parse_uint(value, where + " block size", 1, UINT16_MAX));
        } else if (key == "sign_key_id") {
            index = 2;
            if (!value.empty() && !is_lower_hex(value, 16))
                throw std::runtime_error(where + ": invalid signing key ID");
            state.header.sign_key_id = value;
        } else if (key == "plan_blake3") {
            index = 3;
            if (!value.empty() && !is_lower_hex(value, 64))
                throw std::runtime_error(where + ": invalid plan hash");
            state.header.plan_blake3 = value;
        } else if (key == "archive_name") {
            index = 4;
            state.header.archive_name = value;
        } else {
            throw std::runtime_error(format("{}: unknown record type '{}'",
                                            where,
                                            escape_bytes_for_diagnostic(key)));
        }
        if (seen[index] || !state.points.empty())
            throw std::runtime_error(where + ": repeated header record");
        seen[index] = true;
        state.valid_size += record.size() + 2;
    }
    if (std::ranges::find(seen, false) != seen.end())
        throw std::runtime_error(path.string() +
                                 ": incomplete resume file header");
    return state;
}

string blake3_file_hex(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("open " + path.string());
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::vector<char> buffer(1024UL * 1024UL);
    while (input.read(buffer.data(),
                      static_cast<std::streamsize>(buffer.size())) ||
           input.gcount() > 0)
        blake3_hasher_update(&hasher, buffer.data(),
                             static_cast<size_t>(input.gcount()));
    if (input.bad())
        throw std::runtime_error("read " + path.string());
    std::array<uint8_t, BLAKE3_OUT_LEN> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());
    return hex_encode(hash);
}

ResumeLog::ResumeLog(const fs::path &path, const ResumeHeader &header)
    : path_(path) {
    fd_ = ::open(path.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        if (errno == EEXIST)
            throw std::runtime_error(
                format("resume file {} already exists; resume with "
                       "--resume-from-slice or remove it",
                       path.string()));
        throw_errno(path, "create");
    }
    append(encode_resume_header(header));
    if (fd_ < 0)
        throw std::runtime_error("cannot write resume file " + path.string());
}

ResumeLog::ResumeLog(const fs::path &path, const ResumeState &state,
                     const ResumePoint &restart)
    : path_(path), last_recorded_slice_(restart.slice) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd_ < 0)
        throw_errno(path, "open");
    // Drop a record cut short by a crash so that appended records do not
    // run into it.
    if (::ftruncate(fd_, static_cast<off_t>(state.valid_size)) != 0) {
        int const saved = errno;
        ::close(fd_);
        fd_ = -1;
        errno = saved;
        throw_errno(path, "truncate");
    }
    append(encode_resume_point(restart));
    if (fd_ < 0)
        throw std::runtime_error("cannot write resume file " + path.string());
}

ResumeLog::~ResumeLog() {
    if (fd_ >= 0)
        ::close(fd_);
}

void ResumeLog::append(const string &records) {
    if (fd_ < 0)
        return;
    size_t done = 0;
    bool failed = false;
    while (done < records.size() && !failed) {
        ssize_t const n =
            ::write(fd_, records.data() + done, records.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            failed = true;
        else
            done += static_cast<size_t>(n);
    }
    if (!failed && ::fdatasync(fd_) != 0)
        failed = true;
    if (failed) {
        write_diagnostic(
            format("{}: warning: writing resume file {} failed ({}); later "
                   "slices are not recorded",
                   program_name, path_.string(), std::strerror(errno)));
        ::close(fd_);
        fd_ = -1;
    }
}

void ResumeLog::slice_started(uint64_t slice, uint64_t global_seq) {
    std::scoped_lock const lock(mutex_);
    pending_.push_back({slice, global_seq});
}

void ResumeLog::frame_acked(uint64_t global_seq, uint64_t volume) {
    std::scoped_lock const lock(mutex_);
    while (!pending_.empty() && pending_.front().global_seq <= global_seq) {
        PendingSlice const started = pending_.front();
        pending_.pop_front();
        if (last_recorded_slice_ && started.slice <= *last_recorded_slice_)
            continue;
        append(
            encode_resume_point({started.slice, started.global_seq, volume}));
        last_recorded_slice_ = started.slice;
    }
}

} // namespace neotape
