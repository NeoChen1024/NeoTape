#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_ioctl.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mt {
namespace {

using std::format;
using std::string;
using std::string_view;

namespace fs = std::filesystem;

using neotape::scan_spool_files;
using neotape::spool_file_number;

// 8.3 names: NTnnnnnn.NTS for a tape file, NTnnnnnn.TMP while it is written.
fs::path spool_final_path(const fs::path &root, uint64_t file_num) {
    return root / format("NT{:06}.NTS", file_num);
}

fs::path spool_temp_path(const fs::path &root, uint64_t file_num) {
    return root / format("NT{:06}.TMP", file_num);
}

bool is_spool_temp_name(const fs::path &path) {
    std::string name = path.filename().string();
    std::ranges::transform(name, name.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return name.starts_with("NT") && name.ends_with(".TMP") &&
           name.size() <= 12 &&
           std::string_view(name)
                   .substr(2, name.size() - 6)
                   .find_first_not_of("0123456789") == std::string_view::npos;
}

int open_fd(const fs::path &path, int flags) {
    int const fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0) {
        throw Error(path.string(), "open", errno);
    }
    return fd;
}

// Read up to `size` bytes; fewer only at end of file.
size_t read_stream(int fd, const fs::path &path, std::byte *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t const n = ::read(fd, data + done, size - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw Error(path.string(), "read", errno);
        }
        if (n == 0) {
            break;
        }
        done += static_cast<size_t>(n);
    }
    return done;
}

} // anonymous namespace

// -----------------------------------------------------------------------
// Error
// -----------------------------------------------------------------------

Error::Error(std::string_view device, std::string_view operation, int errnum)
    : std::runtime_error(std::string(device) + ": " + std::string(operation) +
                         ": " + std::strerror(errnum)),
      errnum_(errnum) {}

// -----------------------------------------------------------------------
// Status
// -----------------------------------------------------------------------

Status::Status(long mt_type, long mt_resid, long mt_dsreg, long mt_gstat,
               long mt_erreg, int mt_fileno, int mt_blkno)
    : type_(mt_type), resid_(mt_resid), dsreg_(mt_dsreg), gstat_(mt_gstat),
      erreg_(mt_erreg), fileno_(mt_fileno), blkno_(mt_blkno) {}

bool Status::eof() const noexcept { return (gstat_ & GMT_EOF) != 0; }
bool Status::bot() const noexcept { return (gstat_ & GMT_BOT) != 0; }
bool Status::eot() const noexcept { return (gstat_ & GMT_EOT) != 0; }
bool Status::eod() const noexcept { return (gstat_ & GMT_EOD) != 0; }
bool Status::online() const noexcept { return (gstat_ & GMT_ONLINE) != 0; }

// -----------------------------------------------------------------------
// TapeDevice
// -----------------------------------------------------------------------

TapeDevice::TapeDevice(std::string_view device_path, bool read_write)
    : device_path_(device_path) {
    int const oflags = read_write ? O_RDWR : O_RDONLY;

    fd_ = ::open(device_path_.c_str(), oflags | O_NONBLOCK);
    if (fd_ < 0) {
        throw Error(device_path_, "open", errno);
    }

    struct ::stat st;
    if (::fstat(fd_, &st) < 0) {
        int const e = errno;
        ::close(fd_);
        fd_ = -1;
        throw Error(device_path_, "fstat", e);
    }
    if (!S_ISCHR(st.st_mode)) {
        ::close(fd_);
        fd_ = -1;
        throw Error(device_path_, "not a character device", ENOTTY);
    }
}

TapeDevice::TapeDevice(int fd, std::string_view path)
    : fd_(fd), device_path_(path) {}

TapeDevice::~TapeDevice() {
    try {
        close();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "neotape: %s\n", error.what());
    }
}

void TapeDevice::close() {
    if (fd_ < 0)
        return;
    // close may release the descriptor even when it reports an error. Never
    // retry it, or a later file that reuses this descriptor could be closed.
    int const descriptor = fd_;
    fd_ = -1;
    if (::close(descriptor) < 0)
        throw Error(device_path_, "close", errno);
}

MediaRecord TapeDevice::read_record() {
    if (read_buffer_.empty())
        read_buffer_.resize(neotape::max_block_size);
    for (;;) {
        // The driver's file/block counters establish local progress.
        // MTIOCPOS asks for a physical address that some LTO drives do
        // not support and must not be retried for every record.
        auto const before = status();
        ssize_t n;
        do {
            n = ::read(fd_, read_buffer_.data(), read_size_);
        } while (n < 0 && errno == EINTR);
        int const read_error = errno;
        if (n > 0) {
            // Bootstrap with the maximum buffer, then use the verified
            // archive record size to avoid short-record sense handling
            // and pinning eight MiB for every one-MiB read.
            if (read_size_ == neotape::max_block_size ||
                (n >= static_cast<ssize_t>(neotape::fixed_header_size) &&
                 read_buffer_[9] == std::byte{255})) {
                try {
                    auto const *data =
                        reinterpret_cast<const uint8_t *>(read_buffer_.data());
                    auto const frame = neotape::check_frame(data, n);
                    if (frame.hash_ok)
                        read_size_ =
                            frame.header.channel_type ==
                                    neotape::ChannelType::ARCHIVE_END
                                ? neotape::max_block_size
                                : neotape::decoded_block_size(frame.header);
                } catch (const std::exception &) {
                    // Non-NeoTape prefixes and unavailable headers do not
                    // establish a new record size.
                }
            }
            return {RecordEvent::record,
                    {read_buffer_.begin(), read_buffer_.begin() + n},
                    file_num_,
                    device_path_};
        }
        if (n < 0 && read_error != EIO)
            throw Error(device_path_, "read", read_error);
        auto const after_read = status();
        if (after_read.eod())
            return {RecordEvent::end, {}, file_num_, device_path_};
        if (n == 0)
            return {RecordEvent::filemark, {}, file_num_++, device_path_};
        // EIO is not a filemark. Continue only after confirming physical
        // progress to a known record boundary; never retry in place
        // forever.
        if (before.fileno() < 0 || before.blkno() < 0 ||
            after_read.fileno() != before.fileno() ||
            after_read.blkno() < before.blkno())
            throw Error(device_path_, "read: unknown tape position", EIO);
        auto after = after_read;
        if (after.blkno() == before.blkno()) {
            space_fwd_records(1);
            after = status();
        }
        if (after.fileno() != before.fileno() ||
            after.blkno() <= before.blkno())
            throw Error(device_path_,
                        "read: cannot advance past unreadable block", EIO);
        neotape::write_diagnostic(
            format("neotape: unreadable tape record at file {} block {}; "
                   "resumed at block {}",
                   before.fileno(), before.blkno(), after.blkno()));
    }
}

void TapeDevice::write_record(const void *data, std::size_t size) {
    if (size == 0)
        throw Error(device_path_, "empty record", EINVAL);
    NEOTAPE_DEBUG("[tape {}] write_record begin size={}\n", device_path_, size);
    ssize_t written;
    do {
        written = ::write(fd_, data, size);
    } while (written < 0 && errno == EINTR);
    if (written < 0)
        throw Error(device_path_, "write_record", errno);
    // One successful syscall is one physical record. Retrying a positive
    // short write would append a second record, not complete the first.
    if (static_cast<std::size_t>(written) != size)
        throw Error(device_path_, "short record write", EIO);
    NEOTAPE_DEBUG("[tape {}] write_record done size={}\n", device_path_, size);
}

// -- low-level ioctl dispatch ------------------------------------------

void TapeDevice::do_mtop(int op, int count) {
    mtop mt_com{};
    mt_com.mt_op = static_cast<short>(op);
    mt_com.mt_count = count;

    NEOTAPE_DEBUG("[tape {}] mtop op={} count={}\n", device_path_, op, count);
    if (::ioctl(fd_, MTIOCTOP, &mt_com) < 0) {
        NEOTAPE_DEBUG("[tape {}] mtop op={} count={} error errno={} ({})\n",
                      device_path_, op, count, errno, std::strerror(errno));
        throw Error(device_path_, "ioctl", errno);
    }
    NEOTAPE_DEBUG("[tape {}] mtop op={} count={} done\n", device_path_, op,
                  count);
    if (op == MTFSF)
        file_num_ += static_cast<uint64_t>(count);
}

// -- positioning -------------------------------------------------------

void TapeDevice::rewind() { do_mtop(MTREW, 1); }

// Some drives report EIO for MTEOM after they have already reached EOD;
// the status query tells the two apart.
void TapeDevice::space_to_eod() {
    try {
        do_mtop(MTEOM, 1);
    } catch (const Error &e) {
        if (e.error_code() == EIO) {
            try {
                if (status().eod()) {
                    return;
                }
            } catch (const Error &) {
            }
        }
        throw;
    }
}
void TapeDevice::space_fwd(int count) { do_mtop(MTFSF, count); }
void TapeDevice::space_fwd_records(int count) { do_mtop(MTFSR, count); }

// -- markers -----------------------------------------------------------

void TapeDevice::write_filemark(int count) { do_mtop(MTWEOF, count); }

// -- drive control -----------------------------------------------------

// The Linux st driver treats block size 0 as variable block mode.
void TapeDevice::set_variable_block_mode() { do_mtop(MTSETBLK, 0); }
// -- status queries ----------------------------------------------------

Status TapeDevice::status() { return do_status(); }

Status TapeDevice::do_status() {
    mtget raw{};
    NEOTAPE_DEBUG("[tape {}] status ioctl begin\n", device_path_);
    if (::ioctl(fd_, MTIOCGET, &raw) < 0) {
        NEOTAPE_DEBUG("[tape {}] status ioctl error errno={} ({})\n",
                      device_path_, errno, std::strerror(errno));
        throw Error(device_path_, "status", errno);
    }
    Status st(raw.mt_type, raw.mt_resid, raw.mt_dsreg, raw.mt_gstat,
              raw.mt_erreg, raw.mt_fileno, raw.mt_blkno);
    NEOTAPE_DEBUG("[tape {}] status type=0x{:x} resid={} dsreg=0x{:x} "
                  "gstat=0x{:x} erreg=0x{:x} fileno={} blkno={} "
                  "bot={} eot={} eod={} eof={} online={}\n",
                  device_path_, st.type(), st.resid(), st.dsreg(), st.gstat(),
                  st.erreg(), st.fileno(), st.blkno(), st.bot(), st.eot(),
                  st.eod(), st.eof(), st.online());
    return st;
}

// -----------------------------------------------------------------------
// SpoolTapeWriter
// -----------------------------------------------------------------------

SpoolTapeWriter::SpoolTapeWriter(const fs::path &root)
    : TapeDevice(-1, root.string()), root_(root) {
    fs::create_directories(root_);
    files_ = scan_spool_files(root_);
    // scan_spool_files orders by file number, so the last file is the highest.
    current_file_num_ =
        files_.empty() ? 0 : *spool_file_number(files_.back()) + 1;
    open_next_file();
}

SpoolTapeWriter::~SpoolTapeWriter() {
    if (current_path_.empty())
        return;
    // Records in the pending file were acknowledged, so a trailing tape
    // file never closed with a filemark is committed here. Destructors must
    // not throw; a file that cannot be committed is left in place.
    if (current_record_ > 0) {
        try {
            commit_current_file();
        } catch (const std::exception &error) {
            neotape::write_diagnostic(
                format("neotape: warning: {}; records remain in {}",
                       error.what(), current_path_.string()));
            if (spool_fd_ >= 0)
                ::close(spool_fd_);
        }
        return;
    }
    if (spool_fd_ >= 0)
        ::close(spool_fd_);
    std::error_code ec;
    fs::remove(current_path_, ec);
}

// Start a fresh pending file numbered current_file_num_.
void SpoolTapeWriter::open_next_file() {
    current_path_ = spool_temp_path(root_, current_file_num_);
    spool_fd_ = open_fd(current_path_, O_RDWR | O_CREAT | O_TRUNC);
    current_record_ = 0;
    current_bytes_ = 0;
}

// Make the pending file durable and give it its final name.
void SpoolTapeWriter::commit_current_file() {
    if (spool_fd_ >= 0) {
        if (::fsync(spool_fd_) < 0) {
            throw Error(current_path_.string(), "fsync", errno);
        }
        int const fd = spool_fd_;
        spool_fd_ = -1;
        if (::close(fd) < 0) {
            throw Error(current_path_.string(), "close", errno);
        }
    }
    fs::rename(current_path_, spool_final_path(root_, current_file_num_));
    current_path_.clear();
}

int SpoolTapeWriter::fd() const noexcept { return spool_fd_; }

void SpoolTapeWriter::write_record(const void *data, std::size_t size) {
    if (spool_fd_ < 0) {
        throw Error(device_path(), "write_record", EBADF);
    }

    const auto *p = static_cast<const char *>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        ssize_t const w = ::write(spool_fd_, p, remaining);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            // The record is not written at all: a partial record would be
            // read back as damage, and the writer never acknowledges it.
            int const error = w < 0 ? errno : EIO;
            if (::ftruncate(spool_fd_, static_cast<off_t>(current_bytes_)) == 0)
                ::lseek(spool_fd_, static_cast<off_t>(current_bytes_),
                        SEEK_SET);
            throw Error(device_path(), "write_record", error);
        }
        p += w;
        remaining -= static_cast<std::size_t>(w);
    }
    current_bytes_ += size;
    ++current_record_;
}

void SpoolTapeWriter::do_mtop(int op, int count) {
    if (count <= 0) {
        return;
    }

    switch (op) {
    case MTWEOF: {
        if (count != 1) {
            throw Error(device_path(), "write filemark", ENOTSUP);
        }
        if (spool_fd_ < 0) {
            throw Error(device_path(), "write filemark", EBADF);
        }
        commit_current_file();
        ++current_file_num_;
        open_next_file();
        return;
    }

    case MTREW: {
        // Rewind to BOT: remove every spool file and restart numbering.
        if (spool_fd_ >= 0) {
            ::close(spool_fd_);
            spool_fd_ = -1;
        }
        std::error_code ec;
        if (!current_path_.empty()) {
            fs::remove(current_path_, ec);
            current_path_.clear();
        }
        for (const auto &entry : fs::directory_iterator(root_)) {
            if (entry.is_regular_file() && (spool_file_number(entry.path()) ||
                                            is_spool_temp_name(entry.path())))
                fs::remove(entry.path(), ec);
        }
        files_.clear();
        current_file_num_ = 0;
        open_next_file();
        return;
    }

    case MTEOM:
        if (spool_fd_ < 0) {
            throw Error(device_path(), "space to eod", EBADF);
        }
        if (::lseek(spool_fd_, 0, SEEK_END) < 0) {
            throw Error(device_path(), "lseek", errno);
        }
        return;

    default:
        throw Error(device_path(), "mtop", ENOTSUP);
    }
}

Status SpoolTapeWriter::do_status() {
    long gstat = GMT_ONLINE;
    if (spool_fd_ >= 0) {
        gstat |= current_bytes_ == 0 ? GMT_BOT | GMT_EOD : GMT_EOD;
    }
    // Finalized tape files on disk mean the tape is not empty even when the
    // pending file is.
    if (!files_.empty()) {
        gstat &= ~GMT_EOD;
    }
    return Status(0, 0, 0, gstat, 0, static_cast<int>(current_file_num_),
                  static_cast<int>(current_record_));
}

// -----------------------------------------------------------------------
// SpoolTapeReader
// -----------------------------------------------------------------------

SpoolTapeReader::SpoolTapeReader(const fs::path &root)
    : TapeDevice(-1, root.string()), files_(scan_spool_files(root)) {
    open_file(0);
}

SpoolTapeReader::~SpoolTapeReader() {
    if (spool_fd_ >= 0)
        ::close(spool_fd_);
}

int SpoolTapeReader::fd() const noexcept { return spool_fd_; }

// Open files_[index]; past the last file the reader is at EOD.
void SpoolTapeReader::open_file(std::size_t index) {
    if (spool_fd_ >= 0) {
        ::close(spool_fd_);
        spool_fd_ = -1;
    }
    index_ = index;
    if (index_ >= files_.size()) {
        index_ = files_.size();
        return;
    }
    spool_fd_ = open_fd(files_[index_], O_RDONLY);
    // scan_spool_files only lists names that parse.
    file_num_ = *spool_file_number(files_[index_]);
}

// The open file ended: report the filemark and move to the next file.
MediaRecord SpoolTapeReader::end_of_file() {
    MediaRecord const mark{RecordEvent::filemark,
                           {},
                           file_num_,
                           files_[index_].filename().string()};
    open_file(index_ + 1);
    return mark;
}

MediaRecord SpoolTapeReader::read_record() {
    if (index_ >= files_.size())
        return {RecordEvent::end, {}, file_num_, device_path()};
    const fs::path &path = files_[index_];
    MediaRecord result{RecordEvent::record,
                       std::vector<std::byte>(neotape::fixed_header_size),
                       file_num_, path.filename().string()};
    size_t const got = read_stream(spool_fd_, path, result.record.data(),
                                   neotape::fixed_header_size);
    if (got == 0)
        return end_of_file();
    auto const *data = reinterpret_cast<const uint8_t *>(result.record.data());
    // The first header of an archive establishes the record size; later
    // records keep that framing even when their own header is unreadable.
    if (got == neotape::fixed_header_size && block_size_ == 0)
        block_size_ = neotape::decoded_block_size(
            neotape::parse_fixed_header(data, result.record.size()));
    size_t const size = block_size_;
    if (got == neotape::fixed_header_size) {
        result.record.resize(size);
        data = reinterpret_cast<const uint8_t *>(result.record.data());
    }
    if (got < neotape::fixed_header_size ||
        read_stream(spool_fd_, path, result.record.data() + got, size - got) !=
            size - got) {
        // An interrupted write; the writer never acknowledged it.
        neotape::write_diagnostic(
            format("neotape: warning: ignoring partial record at the end "
                   "of {}",
                   result.source_name));
        return end_of_file();
    }
    // A following archive may use another block size; its first header
    // establishes it again.
    try {
        auto const frame = neotape::check_frame(data, result.record.size());
        if (frame.hash_ok &&
            frame.header.channel_type == neotape::ChannelType::ARCHIVE_END)
            block_size_ = 0;
    } catch (const std::exception &) {
        // Retain known framing across an unavailable header.
    }
    return result;
}

void SpoolTapeReader::write_record(const void *, std::size_t) {
    throw Error(device_path(), "write_record", EROFS);
}

void SpoolTapeReader::do_mtop(int op, int count) {
    if (count <= 0) {
        return;
    }
    if (op != MTFSF) {
        throw Error(device_path(), "mtop", ENOTSUP);
    }
    open_file(index_ + static_cast<std::size_t>(count));
}

Status SpoolTapeReader::do_status() {
    long gstat = GMT_ONLINE;
    if (index_ >= files_.size())
        gstat |= GMT_EOD;
    return Status(0, 0, static_cast<long>(block_size_), gstat, 0,
                  static_cast<int>(file_num_), 0);
}

} // namespace mt
