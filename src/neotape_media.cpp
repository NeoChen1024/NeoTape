#include "neotape/format.hpp"
#include "neotape/media.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
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

FrameHeader read_spool_file_header(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    HeaderBytes bytes{};
    if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        throw std::runtime_error(
            format("short header read from {}", path.string()));
    return parse_fixed_header(bytes.data(), bytes.size());
}

namespace {
struct SpoolName {
    uint64_t number;
    std::optional<uint64_t> slice; // nullopt: archive-end file
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

std::optional<uint64_t> spool_file_number(const fs::path &path) {
    auto const parsed = spool_name(path);
    return parsed ? std::optional(parsed->number) : std::nullopt;
}

std::vector<fs::path> scan_spool_files(const fs::path &root) {
    if (!fs::is_directory(root))
        throw std::runtime_error("spool directory does not exist: " +
                                 root.string());
    std::vector<std::pair<uint64_t, fs::path>> numbered;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        if (auto const number = spool_file_number(entry.path()))
            numbered.emplace_back(*number, entry.path());
    }
    std::ranges::sort(numbered);
    for (std::size_t i = 1; i < numbered.size(); ++i) {
        auto const [previous, previous_path] = numbered[i - 1];
        auto const [current, current_path] = numbered[i];
        if (previous == current)
            throw std::runtime_error(
                format("duplicate spool file number {}: {} and {}", current,
                       previous_path.filename().string(),
                       current_path.filename().string()));
        if (current - previous > 1)
            std::cerr << format(
                "neotape: missing spool file numbers between {} and {}\n",
                previous, current);
    }
    std::vector<fs::path> files;
    files.reserve(numbered.size());
    for (auto &[number, path] : numbered)
        files.push_back(std::move(path));
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
    // scan_spool_files only lists names that parse.
    SpoolName const name = *spool_name(files_[index_]);
    file_num_ = name.number;
    file_slice_ = name.slice;
}

RecordReader::RecordReader(const MediaLocator &source) {
    if (source.kind == MediaLocator::tape) {
        tape_ = std::make_unique<mt::TapeDevice>(source.path, false);
        int flags = ::fcntl(tape_->fd(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(tape_->fd(), F_SETFL, flags & ~O_NONBLOCK) < 0)
            throw std::runtime_error(format("fcntl: {}", std::strerror(errno)));
        // Drives without variable block mode can still be read when their
        // fixed block size matches the recorded block size.
        try {
            tape_->set_variable_block_mode();
        } catch (const mt::Error &e) {
            std::cerr << format(
                "neotape: warning: variable block mode unavailable: {}\n",
                e.what());
        }
        buffer_.resize(max_block_size);
    } else if (source.kind == MediaLocator::spool) {
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
                        auto const frame = check_frame(data, n);
                        if (frame.hash_ok)
                            tape_read_size_ =
                                frame.header.channel_type ==
                                        ChannelType::ARCHIVE_END
                                    ? max_block_size
                                    : decoded_block_size(frame.header);
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
    auto const *data = reinterpret_cast<const uint8_t *>(result.record.data());
    // The first header of an archive establishes the record size; later
    // records keep that framing even when their own header is unreadable.
    if (!spool_block_size_)
        spool_block_size_ =
            decoded_block_size(parse_fixed_header(data, result.record.size()));
    size_t const size = spool_block_size_;
    result.record.resize(size);
    data = reinterpret_cast<const uint8_t *>(result.record.data());
    if (read_stream(spool_fd_, result.record.data() + fixed_header_size,
                    size - fixed_header_size) != size - fixed_header_size)
        throw std::runtime_error("truncated record in " + result.source_name);
    std::optional<FrameHeader> valid_header;
    try {
        auto frame = check_frame(data, result.record.size());
        if (frame.hash_ok)
            valid_header = std::move(frame.header);
    } catch (const std::exception &) {
        // Retain known framing across an unavailable header.
    }
    if (valid_header) {
        bool const end = valid_header->channel_type == ChannelType::ARCHIVE_END;
        if (end == file_slice_.has_value() ||
            (file_slice_ && *file_slice_ != valid_header->slice_seq_num))
            throw std::runtime_error(
                "spool filename disagrees with frame header: " +
                result.source_name);
        if (end)
            spool_block_size_ = 0;
    }
    return result;
}
} // namespace neotape
