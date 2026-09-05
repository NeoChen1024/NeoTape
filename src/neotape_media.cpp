#include "neotape/format.hpp"
#include "neotape/media.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <format>
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

bool parse_spool_file_name(const fs::path &path, uint64_t &number) {
    auto name = path.filename().string();
    constexpr std::string_view prefix = "neotape-";
    if (!name.starts_with(prefix) || !name.ends_with(".nts"))
        return false;
    auto end = name.find('.', prefix.size());
    if (end == std::string::npos || end == prefix.size())
        return false;
    auto [ptr, error] =
        std::from_chars(name.data() + prefix.size(), name.data() + end, number);
    return error == std::errc{} && ptr == name.data() + end;
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
        ssize_t n;
        do {
            n = ::read(tape_->fd(), buffer_.data(), buffer_.size());
        } while (n < 0 && errno == EINTR);
        if (n > 0)
            return {RecordEvent::record,
                    {buffer_.begin(), buffer_.begin() + n},
                    file_num_,
                    tape_->device_path()};
        if (n < 0 && errno != EIO)
            throw mt::Error(tape_->device_path(), "read", errno);
        if (tape_->status().eod()) {
            ended_ = true;
            return {};
        }
        return {RecordEvent::filemark, {}, file_num_++, tape_->device_path()};
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
    auto header = parse_fixed_header(
        reinterpret_cast<const uint8_t *>(result.record.data()),
        result.record.size());
    size_t size = decoded_block_size(header);
    result.record.resize(size);
    if (read_stream(spool_fd_, result.record.data() + fixed_header_size,
                    size - fixed_header_size) != size - fixed_header_size)
        throw std::runtime_error("truncated record in " + result.source_name);
    return result;
}
} // namespace neotape
