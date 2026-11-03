#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_ioctl.hpp"

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

constexpr string_view spool_prefix = "neotape-";
constexpr string_view spool_ext = ".nts";

using neotape::scan_spool_files;
using neotape::spool_file_number;

std::string spool_suffix_for_header(const neotape::FrameHeader &header) {
    switch (header.channel_type) {
    case neotape::ChannelType::CH_CONTENT:
    case neotape::ChannelType::CH_METADATA:
        return format("slice-{:06}", header.slice_seq_num);
    case neotape::ChannelType::ARCHIVE_END:
        return "archive-end";
    }
    throw std::runtime_error("unsupported spool channel type");
}

fs::path spool_final_path(const fs::path &root, uint64_t file_num,
                          const neotape::FrameHeader &header) {
    return root / format("{}{:06}.{}{}", spool_prefix, file_num,
                         spool_suffix_for_header(header), spool_ext);
}

fs::path spool_temp_path(const fs::path &root, uint64_t file_num) {
    return root / format("{}{:06}.pending", spool_prefix, file_num);
}

int open_fd(const fs::path &path, int flags) {
    int const fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0) {
        throw std::runtime_error(
            format("open {}: {}", path.string(), std::strerror(errno)));
    }
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
// SpoolTapeDevice
// -----------------------------------------------------------------------

SpoolTapeDevice::SpoolTapeDevice(const fs::path &root)
    : TapeDevice(-1, root.string()), root_(root) {
    fs::create_directories(root_);
    files_ = scan_spool_files(root_);
    // scan_spool_files orders by file number, so the last file is the highest.
    current_file_num_ =
        files_.empty() ? 0 : *spool_file_number(files_.back()) + 1;
    open_next_file();
}

SpoolTapeDevice::~SpoolTapeDevice() {
    if (spool_fd_ >= 0) {
        ::close(spool_fd_);
        spool_fd_ = -1;
    }
    if (current_path_.empty())
        return;
    // A trailing tape file never closed with a filemark is finalized here;
    // an empty one is removed. Destructors must not throw.
    if (current_record_ > 0) {
        try {
            finalize_current_file();
            return;
        } catch (...) {
        }
    }
    std::error_code ec;
    fs::remove(current_path_, ec);
}

// Start a fresh pending file numbered current_file_num_.
void SpoolTapeDevice::open_next_file() {
    current_path_ = spool_temp_path(root_, current_file_num_);
    spool_fd_ = open_fd(current_path_, O_RDWR | O_CREAT | O_TRUNC);
    current_block_size_ = 0;
    current_record_ = 0;
}

void SpoolTapeDevice::finalize_current_file() {
    auto header = neotape::read_spool_file_header(current_path_);
    current_block_size_ = neotape::decoded_block_size(header);
    fs::path const final_path =
        spool_final_path(root_, current_file_num_, header);
    fs::rename(current_path_, final_path);
    current_path_.clear();
}

int SpoolTapeDevice::fd() const noexcept { return spool_fd_; }

void SpoolTapeDevice::write_record(const void *data, std::size_t size) {
    if (spool_fd_ < 0) {
        throw Error(device_path(), "write_record", EBADF);
    }

    const auto *p = static_cast<const char *>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        ssize_t const w = ::write(spool_fd_, p, remaining);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw Error(device_path(), "write_record", errno);
        }
        if (w == 0) {
            throw Error(device_path(), "write_record", EIO);
        }
        p += w;
        remaining -= static_cast<std::size_t>(w);
    }
    ++current_record_;
}

void SpoolTapeDevice::do_mtop(int op, int count) {
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

        if (::fsync(spool_fd_) < 0) {
            throw Error(device_path(), "fsync", errno);
        }
        if (::close(spool_fd_) < 0) {
            throw Error(device_path(), "close", errno);
        }
        spool_fd_ = -1;

        finalize_current_file();
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
        for (auto &entry : fs::directory_iterator(root_)) {
            if (!entry.is_regular_file())
                continue;
            auto name = entry.path().filename().string();
            if (name.size() <= spool_prefix.size() + spool_ext.size() ||
                !name.starts_with(spool_prefix) || !name.ends_with(spool_ext)) {
                continue;
            }
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

Status SpoolTapeDevice::do_status() {
    long gstat = GMT_ONLINE;
    if (spool_fd_ >= 0) {
        off_t const cur = ::lseek(spool_fd_, 0, SEEK_CUR);
        off_t const end = ::lseek(spool_fd_, 0, SEEK_END);
        if (cur >= 0) {
            ::lseek(spool_fd_, cur, SEEK_SET);
        }
        if (end == 0) {
            gstat |= GMT_BOT | GMT_EOD;
        } else {
            gstat |= GMT_EOD;
        }
    }
    // Finalized tape files on disk mean the tape is not empty even when the
    // pending file is.
    if (!files_.empty()) {
        gstat &= ~GMT_EOD;
    }
    return Status(0, 0, static_cast<long>(current_block_size_), gstat, 0,
                  static_cast<int>(current_file_num_),
                  static_cast<int>(current_record_));
}

} // namespace mt
