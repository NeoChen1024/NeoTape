#include "neotape/format.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_ioctl.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mt {
namespace {

using std::format;
using std::map;
using std::string;
using std::string_view;

namespace fs = std::filesystem;

// -----------------------------------------------------------------------
// Minimal density table — independently researched from public specs.
// Covers modern LTO formats.
// -----------------------------------------------------------------------

const map<int, string> density_names = {
    {0x0, "default"},        {0x41, "LTO-2 Ultrium"}, {0x42, "LTO-2 Ultrium"},
    {0x44, "LTO-3 Ultrium"}, {0x46, "LTO-4 Ultrium"}, {0x58, "LTO-5 Ultrium"},
    {0x5a, "LTO-6 Ultrium"}, {0x5c, "LTO-7 Ultrium"}, {0x5d, "LTO-7 M8"},
    {0x5e, "LTO-8 Ultrium"}, {0x60, "LTO-9 Ultrium"},
};

constexpr string_view spool_prefix = "tape-file-";
constexpr string_view spool_ext = ".nts";
constexpr string_view spool_temp_ext = ".pending";

const char *density_name_for_code(int code) noexcept {
    auto it = density_names.find(code);
    if (it == density_names.end())
        return "unknown";
    return it->second.c_str();
}

bool parse_spool_file_name(const fs::path &path, uint64_t &file_num) {
    string name = path.filename().string();
    if (name.size() <= spool_prefix.size() + spool_ext.size() ||
        name.rfind(spool_prefix, 0) != 0 ||
        name.substr(name.size() - spool_ext.size()) != spool_ext)
        return false;

    size_t number_begin = spool_prefix.size();
    size_t number_end = name.find('.', number_begin);
    if (number_end == string::npos || number_end == number_begin)
        return false;
    string_view middle(name.c_str() + number_begin, number_end - number_begin);
    char *end = nullptr;
    file_num = std::strtoull(middle.data(), &end, 10);
    return end != nullptr && end == middle.data() + middle.size();
}

std::vector<fs::path> scan_spool_files(const fs::path &root) {
    std::vector<fs::path> files;
    if (!fs::exists(root))
        return files;

    for (const auto &entry : fs::directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        uint64_t file_num = 0;
        if (!parse_spool_file_name(entry.path(), file_num))
            continue;
        files.push_back(entry.path());
    }

    std::ranges::sort(files, [](const fs::path &a, const fs::path &b) {
        uint64_t an = 0;
        uint64_t bn = 0;
        parse_spool_file_name(a, an);
        parse_spool_file_name(b, bn);
        return an < bn;
    });
    return files;
}

std::string spool_suffix_for_header(const neotape::ParsedHeader &header) {
    switch (header.type) {
    case neotape::HeaderType::volume:
        return "volume-header";
    case neotape::HeaderType::frame:
        return format("slice-{:06}", header.frame->logical_slice_seq_num);
    case neotape::HeaderType::archive_end:
        return "archive-end";
    default:
        throw std::runtime_error("unsupported spool header type");
    }
}

fs::path spool_final_path(const fs::path &root, uint64_t file_num,
                          const neotape::ParsedHeader &header) {
    return root / format("{}{:06}.{}.nts", spool_prefix, file_num,
                         spool_suffix_for_header(header));
}

fs::path spool_temp_path(const fs::path &root, uint64_t file_num) {
    return root / format("{}{:06}.pending", spool_prefix, file_num);
}

neotape::ParsedHeader parse_spool_header_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error(format("open {}", path.string()));

    std::vector<uint8_t> bytes(neotape::fixed_header_size);
    if (!in.read(reinterpret_cast<char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error(format("short read from {}", path.string()));
    return neotape::parse_fixed_header(bytes.data(), bytes.size());
}

int open_fd(const fs::path &path, int flags) {
    int fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0)
        throw std::runtime_error(
            format("open {}: {}", path.string(), std::strerror(errno)));
    return fd;
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

bool Status::eof() const noexcept { return gstat_ & GMT_EOF; }
bool Status::bot() const noexcept { return gstat_ & GMT_BOT; }
bool Status::eot() const noexcept { return gstat_ & GMT_EOT; }
bool Status::sm() const noexcept { return gstat_ & GMT_SM; }
bool Status::eod() const noexcept { return gstat_ & GMT_EOD; }
bool Status::wr_prot() const noexcept { return gstat_ & GMT_WR_PROT; }
bool Status::online() const noexcept { return gstat_ & GMT_ONLINE; }
bool Status::dr_open() const noexcept { return gstat_ & GMT_DR_OPEN; }
bool Status::cleaning_requested() const noexcept { return gstat_ & GMT_CLN; }

int Status::density_code() const noexcept {
    return static_cast<int>((dsreg_ & MT_ST_DENSITY_MASK) >>
                            MT_ST_DENSITY_SHIFT);
}

int Status::block_size() const noexcept {
    return static_cast<int>((dsreg_ & MT_ST_BLKSIZE_MASK) >>
                            MT_ST_BLKSIZE_SHIFT);
}

std::string_view Status::density_name() const {
    auto *n = density_name_for_code(density_code());
    return n ? std::string_view(n) : std::string_view("unknown");
}

std::string Status::type_name() const {
    if (type_ == MT_ISSCSI1)
        return "SCSI 1";
    if (type_ == MT_ISSCSI2)
        return "SCSI 2";
    if (type_ == MT_ISONSTREAM_SC)
        return "OnStream SC-, DI-, DP-, or USB";
    if (type_ & 0x800000)
        return "qic-117 drive type = 0x" + std::to_string(type_ & 0x1ffff);
    if (type_ == 0)
        return "IDE-Tape (type code 0) ?";
    return "Unknown tape drive type (code " + std::to_string(type_) + ")";
}

// -----------------------------------------------------------------------
// TapeDevice
// -----------------------------------------------------------------------

TapeDevice::TapeDevice(std::string_view device_path, bool read_write)
    : device_path_(device_path), read_write_(read_write) {
    int oflags = read_write ? O_RDWR : O_RDONLY;

    fd_ = ::open(device_path_.c_str(), oflags | O_NONBLOCK);
    if (fd_ < 0)
        throw Error(device_path_, "open", errno);

    struct ::stat st;
    if (::fstat(fd_, &st) < 0) {
        int e = errno;
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

TapeDevice::TapeDevice(int fd, std::string_view path, bool read_write)
    : fd_(fd), device_path_(path), read_write_(read_write) {}

TapeDevice::~TapeDevice() {
    if (fd_ >= 0)
        ::close(fd_);
}

TapeDevice::TapeDevice(TapeDevice &&other) noexcept
    : fd_(other.fd_), device_path_(std::move(other.device_path_)),
      read_write_(other.read_write_) {
    other.fd_ = -1;
}

TapeDevice &TapeDevice::operator=(TapeDevice &&other) noexcept {
    if (this != &other) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.fd_;
        device_path_ = std::move(other.device_path_);
        read_write_ = other.read_write_;
        other.fd_ = -1;
    }
    return *this;
}

void TapeDevice::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void TapeDevice::write_record(const void *data, std::size_t size) {
    const auto *p = static_cast<const char *>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        ssize_t w = ::write(fd_, p, remaining);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            throw Error(device_path_, "write_record", errno);
        }
        if (w == 0)
            throw Error(device_path_, "write_record", EIO);
        p += w;
        remaining -= static_cast<std::size_t>(w);
    }
}

void TapeDevice::reopen() {
    close();
    int oflags = read_write_ ? O_RDWR : O_RDONLY;
    fd_ = ::open(device_path_.c_str(), oflags | O_NONBLOCK);
    if (fd_ < 0)
        throw Error(device_path_, "open", errno);
}

// -- low-level ioctl dispatch ------------------------------------------

void TapeDevice::do_mtop(int op, int count) {
    mtop mt_com{};
    mt_com.mt_op = static_cast<short>(op);
    mt_com.mt_count = count;

    if (::ioctl(fd_, MTIOCTOP, &mt_com) < 0)
        throw Error(device_path_, "ioctl", errno);
}

// -- positioning -------------------------------------------------------

void TapeDevice::rewind() { do_mtop(MTREW, 1); }
void TapeDevice::space_to_eod() {
    try {
        do_mtop(MTEOM, 1);
    } catch (const Error &e) {
        if (e.error_code() == EIO) {
            try {
                if (status().eod())
                    return;
            } catch (const Error &) {
            }
        }
        throw;
    }
}
void TapeDevice::space_fwd(int count) { do_mtop(MTFSF, count); }
void TapeDevice::space_bwd(int count) { do_mtop(MTBSF, count); }
void TapeDevice::space_fwd_filemark(int cnt) { do_mtop(MTFSFM, cnt); }
void TapeDevice::space_bwd_filemark(int cnt) { do_mtop(MTBSFM, cnt); }
void TapeDevice::space_fwd_records(int cnt) { do_mtop(MTFSR, cnt); }
void TapeDevice::space_bwd_records(int cnt) { do_mtop(MTBSR, cnt); }
void TapeDevice::space_fwd_setmarks(int cnt) { do_mtop(MTFSS, cnt); }
void TapeDevice::space_bwd_setmarks(int cnt) { do_mtop(MTBSS, cnt); }

void TapeDevice::seek_block(long block_no) {
    do_mtop(MTSEEK, static_cast<int>(block_no));
}

Position TapeDevice::tell() { return do_tell(); }

// -- markers -----------------------------------------------------------

void TapeDevice::write_filemark(int count) { do_mtop(MTWEOF, count); }
void TapeDevice::write_setmark(int count) { do_mtop(MTWSM, count); }
void TapeDevice::erase(int count) { do_mtop(MTERASE, count); }

// -- drive control -----------------------------------------------------

void TapeDevice::set_block_size(int bytes) { do_mtop(MTSETBLK, bytes); }
TapeBlockModeResult TapeDevice::configure_preferred_variable_block_mode(
    uint32_t fallback_block_size, std::string_view context,
    std::ostream &warnings) {
    try {
        set_block_size(0);
        return TapeBlockModeResult{TapeBlockMode::variable, 0};
    } catch (const Error &e) {
        warnings << format(
            "{}: variable block mode unavailable: {}; falling back "
            "to fixed block mode {} bytes\n",
            context, e.what(), fallback_block_size);
        set_block_size(static_cast<int>(fallback_block_size));
        return TapeBlockModeResult{TapeBlockMode::fixed, fallback_block_size};
    }
}
void TapeDevice::set_density(int code) { do_mtop(MTSETDENSITY, code); }
void TapeDevice::set_compression(bool en) {
    do_mtop(MTCOMPRESSION, en ? 1 : 0);
}
void TapeDevice::lock() { do_mtop(MTLOCK, 1); }
void TapeDevice::unlock() { do_mtop(MTUNLOCK, 1); }
void TapeDevice::load(int count) { do_mtop(MTLOAD, count); }
void TapeDevice::offline() { do_mtop(MTOFFL, 1); }

// -- status queries ----------------------------------------------------

Status TapeDevice::status() { return do_status(); }

bool TapeDevice::is_online() { return status().online(); }
bool TapeDevice::is_write_protected() { return status().wr_prot(); }
int TapeDevice::get_fileno() { return status().fileno(); }
int TapeDevice::get_blkno() { return status().blkno(); }

// -- virtual (real implementation) -------------------------------------

Position TapeDevice::do_tell() {
    mtpos pos{};
    if (::ioctl(fd_, MTIOCPOS, &pos) < 0)
        throw Error(device_path_, "tell", errno);
    return Position{pos.mt_blkno};
}

Status TapeDevice::do_status() {
    mtget raw{};
    if (::ioctl(fd_, MTIOCGET, &raw) < 0)
        throw Error(device_path_, "status", errno);
    return Status(raw.mt_type, raw.mt_resid, raw.mt_dsreg, raw.mt_gstat,
                  raw.mt_erreg, static_cast<int>(raw.mt_fileno),
                  static_cast<int>(raw.mt_blkno));
}

// -- static helpers ----------------------------------------------------

std::string_view TapeDevice::density_name(int code) {
    auto *n = density_name_for_code(code);
    return n ? std::string_view(n) : std::string_view("unknown");
}

SpoolTapeDevice::SpoolTapeDevice(const fs::path &root, bool read_write)
    : TapeDevice(-1, root.string(), read_write), root_(root),
      read_write_(read_write) {
    if (read_write_)
        fs::create_directories(root_);

    files_ = scan_spool_files(root_);
    for (const auto &file : files_) {
        uint64_t file_num = 0;
        if (parse_spool_file_name(file, file_num) && file_num >= next_file_num_)
            next_file_num_ = file_num + 1;
    }

    if (read_write_) {
        current_file_num_ = next_file_num_;
        current_path_ = spool_temp_path(root_, current_file_num_);
        spool_fd_ = open_fd(current_path_, O_RDWR | O_CREAT | O_TRUNC);
        current_is_temp_ = true;
        return;
    }

    if (!files_.empty()) {
        current_file_num_ = 0;
        parse_spool_file_name(files_.front(), current_file_num_);
        current_path_ = files_.front();
        spool_fd_ = open_fd(current_path_, O_RDONLY);
        current_block_size_ = 0;
    }
}

SpoolTapeDevice::~SpoolTapeDevice() {
    if (spool_fd_ >= 0) {
        ::close(spool_fd_);
        spool_fd_ = -1;
    }
    if (read_write_ && current_is_temp_ && current_record_ > 0) {
        // Finalize any trailing tape file that was never explicitly closed
        // with a filemark.  Destructors must not throw, so swallow errors.
        try {
            finalize_current_file();
        } catch (...) {
            std::error_code ec;
            fs::remove(current_path_, ec);
        }
    } else if (read_write_ && current_is_temp_ && !current_path_.empty()) {
        std::error_code ec;
        fs::remove(current_path_, ec);
    }
}

void SpoolTapeDevice::finalize_current_file() {
    if (!current_is_temp_ || current_path_.empty())
        return;

    auto header = parse_spool_header_file(current_path_);
    if (header.volume)
        current_block_size_ = header.volume->volume_block_size;
    else if (header.frame)
        current_block_size_ = header.frame->volume_block_size;
    else if (header.archive_end)
        current_block_size_ = header.archive_end->volume_block_size;
    fs::path final_path = spool_final_path(root_, current_file_num_, header);
    fs::rename(current_path_, final_path);
    current_is_temp_ = false;
    current_path_.clear();
}

int SpoolTapeDevice::fd() const noexcept { return spool_fd_; }

void SpoolTapeDevice::write_record(const void *data, std::size_t size) {
    if (!read_write_)
        throw Error(device_path(), "write_record", EROFS);
    if (spool_fd_ < 0)
        throw Error(device_path(), "write_record", EBADF);

    const auto *p = static_cast<const char *>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        ssize_t w = ::write(spool_fd_, p, remaining);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            throw Error(device_path(), "write_record", errno);
        }
        if (w == 0)
            throw Error(device_path(), "write_record", EIO);
        p += w;
        remaining -= static_cast<std::size_t>(w);
    }
    ++current_record_;
}

void SpoolTapeDevice::do_mtop(int op, int count) {
    if (count <= 0)
        return;

    switch (op) {
    case MTSETBLK:
        current_block_size_ = static_cast<uint32_t>(count);
        return;

    case MTWEOF: {
        if (!read_write_)
            throw Error(device_path(), "write filemark", ENOTSUP);
        if (count != 1)
            throw Error(device_path(), "write filemark", ENOTSUP);
        if (spool_fd_ < 0)
            throw Error(device_path(), "write filemark", EBADF);

        if (::fsync(spool_fd_) < 0)
            throw Error(device_path(), "fsync", errno);
        if (::close(spool_fd_) < 0)
            throw Error(device_path(), "close", errno);
        spool_fd_ = -1;

        finalize_current_file();

        ++next_file_num_;
        current_file_num_ = next_file_num_;
        current_path_ = spool_temp_path(root_, current_file_num_);
        spool_fd_ = open_fd(current_path_, O_RDWR | O_CREAT | O_TRUNC);
        current_is_temp_ = true;
        current_block_size_ = 0;
        current_record_ = 0;
        return;
    }

    case MTFSF:
    case MTFSFM: {
        if (read_write_)
            throw Error(device_path(), "filemark spacing", ENOTSUP);
        if (spool_fd_ >= 0) {
            ::close(spool_fd_);
            spool_fd_ = -1;
        }

        auto it = std::ranges::find(files_, current_path_);
        size_t index =
            it == files_.end()
                ? 0
                : static_cast<size_t>(std::distance(files_.begin(), it)) + 1;
        if (index >= files_.size()) {
            current_path_.clear();
            return;
        }

        current_path_ = files_[index];
        parse_spool_file_name(current_path_, current_file_num_);
        spool_fd_ = open_fd(current_path_, O_RDONLY);
        current_record_ = 0;
        current_block_size_ = 0;
        return;
    }

    case MTREW:
        if (spool_fd_ < 0)
            throw Error(device_path(), "rewind", EBADF);
        if (::lseek(spool_fd_, 0, SEEK_SET) < 0)
            throw Error(device_path(), "lseek", errno);
        current_record_ = 0;
        return;

    case MTEOM:
        if (spool_fd_ < 0)
            throw Error(device_path(), "space to eod", EBADF);
        if (::lseek(spool_fd_, 0, SEEK_END) < 0)
            throw Error(device_path(), "lseek", errno);
        return;

    case MTBSF:
    case MTBSFM:
        throw Error(device_path(), "filemark spacing", ENOTSUP);

    case MTFSR:
    case MTBSR:
        throw Error(device_path(), "record spacing", ENOTSUP);

    case MTERASE:
        return;

    default:
        throw Error(device_path(), "mtop", ENOTSUP);
    }
}

Position SpoolTapeDevice::do_tell() {
    if (spool_fd_ < 0)
        throw Error(device_path(), "tell", EBADF);
    off_t pos = ::lseek(spool_fd_, 0, SEEK_CUR);
    if (pos < 0)
        throw Error(device_path(), "tell", errno);
    return Position{static_cast<long>(pos)};
}

Status SpoolTapeDevice::do_status() {
    long gstat = GMT_ONLINE;
    if (read_write_) {
        if (current_is_temp_ && spool_fd_ >= 0) {
            off_t cur = ::lseek(spool_fd_, 0, SEEK_CUR);
            off_t end = ::lseek(spool_fd_, 0, SEEK_END);
            if (cur >= 0)
                ::lseek(spool_fd_, cur, SEEK_SET);
            if (end == 0)
                gstat |= GMT_BOT | GMT_EOD;
            else
                gstat |= GMT_EOD;
        }
        // If there are finalized tape files on disk, the tape is no longer
        // empty even if the current temp file is empty.
        if (!files_.empty())
            gstat &= ~GMT_EOD;
    }
    return Status(0, 0, static_cast<long>(current_block_size_), gstat, 0,
                  static_cast<int>(current_file_num_),
                  static_cast<int>(current_record_));
}

bool TapeDevice::is_scsi_tape(int fd) {
    mtget raw{};
    if (::ioctl(fd, MTIOCGET, &raw) < 0)
        return false;
    return raw.mt_type == MT_ISSCSI1 || raw.mt_type == MT_ISSCSI2;
}

} // namespace mt
