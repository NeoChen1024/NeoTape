#include "neotape/format.hpp"
#include "neotape/media.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <optional>
#include <unistd.h>

namespace neotape {
namespace fs = std::filesystem;
using std::format;

MediaLocator parse_media(std::string_view text, bool allow_null) {
    if (allow_null && text == "null")
        return {MediaLocator::null_sink, {}};
    for (auto [prefix, kind] : {std::pair{"tape:", MediaLocator::tape},
                                {"spool:", MediaLocator::spool}}) {
        if (text.starts_with(prefix) && text.size() > std::strlen(prefix))
            return {kind, std::string(text.substr(std::strlen(prefix)))};
    }
    throw std::invalid_argument(
        allow_null ? "target must be tape:<device>, spool:<dir>, or null"
                   : "source must be tape:<device> or spool:<dir>");
}

namespace {
struct SpoolName {
    uint64_t number;
    std::optional<uint64_t> slice;
};

std::optional<SpoolName> spool_name(const fs::path &path) {
    std::string const name = path.filename().string();
    constexpr std::string_view prefix = "neotape-";
    if (!name.starts_with(prefix) || !name.ends_with(".nts"))
        return std::nullopt;
    std::string_view rest(name.data() + prefix.size(),
                          name.size() - prefix.size() - 4);
    auto number = [](std::string_view &text) -> std::optional<uint64_t> {
        auto end = text.find_first_not_of("0123456789");
        if (end == std::string_view::npos)
            end = text.size();
        if (end == 0)
            return std::nullopt;
        uint64_t result = 0;
        auto [ptr, error] =
            std::from_chars(text.data(), text.data() + end, result);
        if (error == std::errc::result_out_of_range)
            throw std::runtime_error("spool filename number overflow");
        if (error != std::errc{})
            return std::nullopt;
        text.remove_prefix(end);
        return result;
    };
    auto file = number(rest);
    if (!file || !rest.starts_with("."))
        return std::nullopt;
    rest.remove_prefix(1);
    std::optional<uint64_t> slice;
    if (rest.starts_with("slice-")) {
        rest.remove_prefix(6);
        slice = number(rest);
        if (!slice)
            return std::nullopt;
    } else if (rest.starts_with("archive-end")) {
        rest.remove_prefix(11);
    } else {
        return std::nullopt;
    }
    if (!rest.empty()) {
        if (!rest.starts_with("-") || rest.size() == 1)
            return std::nullopt;
        rest.remove_prefix(1);
        for (char c : rest)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return std::nullopt;
    }
    return SpoolName{*file, slice};
}
} // namespace

bool parse_spool_file_name(const fs::path &path, uint64_t &number) {
    auto parsed = spool_name(path);
    if (!parsed)
        return false;
    number = parsed->number;
    return true;
}

std::vector<fs::path> scan_spool_files(const fs::path &root) {
    std::vector<fs::path> files;
    if (!fs::exists(root))
        return files;
    for (const auto &entry : fs::directory_iterator(root)) {
        uint64_t n;
        if (entry.is_regular_file() && parse_spool_file_name(entry.path(), n))
            files.push_back(entry.path());
    }
    std::ranges::sort(files, [](const auto &a, const auto &b) {
        uint64_t x = 0, y = 0;
        parse_spool_file_name(a, x);
        parse_spool_file_name(b, y);
        return x != y ? x < y : a < b;
    });
    for (std::size_t i = 1; i < files.size(); ++i) {
        uint64_t previous = 0, current = 0;
        parse_spool_file_name(files[i - 1], previous);
        parse_spool_file_name(files[i], current);
        if (previous == current)
            throw std::runtime_error(
                format("duplicate spool file number {}: {} and {}", current,
                       files[i - 1].filename().string(),
                       files[i].filename().string()));
        if (current - previous > 1)
            std::cerr << format(
                "neotape: missing spool file numbers between {} and {}\n",
                previous, current);
    }
    return files;
}

namespace {
size_t read_stream(int fd, std::byte *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = ::read(fd, data + done, size - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                format("read spool: {}", std::strerror(errno)));
        }
        if (n == 0)
            break;
        done += n;
    }
    return done;
}
} // namespace

void RecordReader::open_spool() {
    if (index_ == files_.size()) {
        ended_ = true;
        return;
    }
    spool_fd_ = ::open(files_[index_].c_str(), O_RDONLY);
    if (spool_fd_ < 0)
        throw std::runtime_error(format("open {}: {}", files_[index_].string(),
                                        std::strerror(errno)));
    parse_spool_file_name(files_[index_], file_num_);
}

RecordReader::RecordReader(const MediaLocator &source) {
    if (source.kind == MediaLocator::tape) {
        tape_ = std::make_unique<mt::TapeDevice>(source.path, false);
        int flags = ::fcntl(tape_->fd(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(tape_->fd(), F_SETFL, flags & ~O_NONBLOCK) < 0)
            throw std::runtime_error(format("fcntl: {}", std::strerror(errno)));
        tape_->rewind();
        buffer_.resize(max_block_size);
    } else if (source.kind == MediaLocator::spool) {
        if (!fs::exists(source.path))
            throw std::runtime_error("spool directory does not exist: " +
                                     source.path);
        files_ = scan_spool_files(source.path);
        open_spool();
    } else
        throw std::invalid_argument("record reader requires tape or spool");
}

RecordReader::~RecordReader() {
    if (spool_fd_ >= 0)
        ::close(spool_fd_);
}

void RecordReader::skip_file() {
    if (ended_)
        return;
    if (tape_) {
        try {
            tape_->space_fwd();
            ++file_num_;
        } catch (const mt::Error &) {
            if (!tape_->status().eod())
                throw;
            ended_ = true;
        }
    } else {
        if (spool_fd_ >= 0)
            ::close(spool_fd_);
        spool_fd_ = -1;
        ++index_;
        open_spool();
    }
}

MediaRecord RecordReader::next() {
    if (ended_)
        return {};
    if (tape_) {
        for (;;) {
            // The driver's file/block counters establish local progress.
            // MTIOCPOS asks for a physical address that some LTO drives do
            // not support and must not be retried for every record.
            auto const before = tape_->status();
            ssize_t n;
            do {
                n = ::read(tape_->fd(), buffer_.data(), tape_read_size_);
            } while (n < 0 && errno == EINTR);
            int const read_error = errno;
            if (n > 0) {
                // Bootstrap with the maximum buffer, then use the verified
                // archive record size to avoid short-record sense handling
                // and pinning eight MiB for every one-MiB read.
                if (tape_read_size_ == max_block_size ||
                    (n >= static_cast<ssize_t>(fixed_header_size) &&
                     buffer_[9] == std::byte{255})) {
                    try {
                        auto const *data =
                            reinterpret_cast<const uint8_t *>(buffer_.data());
                        auto const header = parse_fixed_header(data, n);
                        if (verify_frame_hash(data, n, header.frame_hash))
                            tape_read_size_ =
                                header.channel_type == ChannelType::ARCHIVE_END
                                    ? max_block_size
                                    : decoded_block_size(header);
                    } catch (const std::exception &) {
                        // Non-NeoTape prefixes and unavailable headers do not
                        // establish a new record size.
                    }
                }
                return {RecordEvent::record,
                        {buffer_.begin(), buffer_.begin() + n},
                        file_num_,
                        tape_->device_path()};
            }
            if (n < 0 && read_error != EIO)
                throw mt::Error(tape_->device_path(), "read", read_error);
            auto const status = tape_->status();
            if (status.eod()) {
                ended_ = true;
                return {};
            }
            if (n == 0)
                return {RecordEvent::filemark,
                        {},
                        file_num_++,
                        tape_->device_path()};
            // EIO is not a filemark. Continue only after confirming physical
            // progress to a known record boundary; never retry in place
            // forever.
            if (before.fileno() < 0 || before.blkno() < 0 ||
                status.fileno() != before.fileno() ||
                status.blkno() < before.blkno())
                throw mt::Error(tape_->device_path(),
                                "read: unknown tape position", EIO);
            auto after = status;
            if (after.blkno() == before.blkno()) {
                tape_->space_fwd_records(1);
                after = tape_->status();
            }
            if (after.fileno() != before.fileno() ||
                after.blkno() <= before.blkno())
                throw mt::Error(tape_->device_path(),
                                "read: cannot advance past unreadable block",
                                EIO);
            std::cerr << format("neotape: unreadable tape record at file {} "
                                "block {}; resumed at block {}\n",
                                before.fileno(), before.blkno(), after.blkno());
        }
    }
    MediaRecord result{RecordEvent::record,
                       std::vector<std::byte>(fixed_header_size), file_num_,
                       files_[index_].filename().string()};
    size_t n = read_stream(spool_fd_, result.record.data(), fixed_header_size);
    if (n == 0) {
        result.event = RecordEvent::filemark;
        result.record.clear();
        skip_file();
        return result;
    }
    if (n != fixed_header_size)
        throw std::runtime_error("truncated header in " + result.source_name);
    size_t size = spool_block_size_;
    try {
        auto header = parse_fixed_header(
            reinterpret_cast<const uint8_t *>(result.record.data()),
            result.record.size());
        if (!spool_block_size_)
            spool_block_size_ = decoded_block_size(header);
        size = spool_block_size_;
    } catch (...) {
        if (!spool_block_size_)
            throw;
        size = spool_block_size_;
    }
    result.record.resize(size);
    if (read_stream(spool_fd_, result.record.data() + fixed_header_size,
                    size - fixed_header_size) != size - fixed_header_size)
        throw std::runtime_error("truncated record in " + result.source_name);
    std::optional<FrameHeader> valid_header;
    try {
        auto const *data =
            reinterpret_cast<const uint8_t *>(result.record.data());
        auto header = parse_fixed_header(data, result.record.size());
        if (verify_frame_hash(data, result.record.size(), header.frame_hash))
            valid_header = std::move(header);
    } catch (const std::exception &) {
        // Retain known framing across an unavailable header.
    }
    if (valid_header) {
        auto name = spool_name(files_[index_]);
        bool const end = valid_header->channel_type == ChannelType::ARCHIVE_END;
        if (end == name->slice.has_value() ||
            (name->slice && *name->slice != valid_header->slice_seq_num))
            throw std::runtime_error(
                "spool filename disagrees with frame header: " +
                result.source_name);
        if (end)
            spool_block_size_ = 0;
    }
    return result;
}
} // namespace neotape
