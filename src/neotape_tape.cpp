#include "neotape/tape.hpp"
#include "neotape/tape_ioctl.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mt {
namespace {

// -----------------------------------------------------------------------
// Minimal density table — independently researched from public specs.
// Covers modern LTO, IBM 3592, and DLT/SDLT formats.
// -----------------------------------------------------------------------

const char *density_name_for_code(int code) noexcept {
    static constexpr struct { int code; const char *name; } tbl[] = {
        { 0x40, "LTO-1 Ultrium"                },
        { 0x41, "LTO-2 Ultrium"                },
        { 0x42, "LTO-3 Ultrium"                },
        { 0x44, "LTO-4 Ultrium"                },
        { 0x46, "LTO-5 Ultrium"                },
        { 0x47, "DDS-5 / DAT-72"               },
        { 0x48, "SDLT-220"                     },
        { 0x49, "SDLT-320"                     },
        { 0x4a, "SDLT-600 / T10000A"           },
        { 0x4b, "T10000B"                      },
        { 0x4c, "T10000C"                      },
        { 0x4d, "T10000D"                      },
        { 0x51, "IBM 3592 J1A"                 },
        { 0x52, "IBM 3592 E05 (TS1120)"        },
        { 0x53, "IBM 3592 E06 (TS1130)"        },
        { 0x54, "IBM 3592 E07 (TS1140)"        },
        { 0x55, "IBM 3592 E08 (TS1150)"        },
        { 0x56, "IBM 3592 55F (TS1155)"        },
        { 0x57, "IBM 3592 60F (TS1160)"        },
        { 0x58, "LTO-6 Ultrium"                },
        { 0x59, "IBM 3592 70F (TS1170)"        },
        { 0x5a, "LTO-7 Ultrium"                },
        { 0x5c, "LTO-8 Ultrium"                },
        { 0x5d, "LTO-7 M8"                     },
        { 0x5e, "LTO-9 Ultrium"                },
    };
    for (auto &e : tbl)
        if (e.code == code)
            return e.name;
    return nullptr;
}

} // anonymous namespace

// -----------------------------------------------------------------------
// Error
// -----------------------------------------------------------------------

Error::Error(std::string_view device, std::string_view operation, int errnum)
    : std::runtime_error(
          std::string(device) + ": " + std::string(operation) + ": " +
          std::strerror(errnum)),
      errnum_(errnum)
{
}

// -----------------------------------------------------------------------
// Status
// -----------------------------------------------------------------------

Status::Status(long mt_type, long mt_resid, long mt_dsreg,
               long mt_gstat, long mt_erreg,
               int mt_fileno, int mt_blkno)
    : type_(mt_type), resid_(mt_resid), dsreg_(mt_dsreg),
      gstat_(mt_gstat), erreg_(mt_erreg),
      fileno_(mt_fileno), blkno_(mt_blkno)
{
}

bool Status::eof()   const noexcept { return gstat_ & GMT_EOF; }
bool Status::bot()   const noexcept { return gstat_ & GMT_BOT; }
bool Status::eot()   const noexcept { return gstat_ & GMT_EOT; }
bool Status::sm()    const noexcept { return gstat_ & GMT_SM; }
bool Status::eod()   const noexcept { return gstat_ & GMT_EOD; }
bool Status::wr_prot()  const noexcept { return gstat_ & GMT_WR_PROT; }
bool Status::online()   const noexcept { return gstat_ & GMT_ONLINE; }
bool Status::dr_open()  const noexcept { return gstat_ & GMT_DR_OPEN; }
bool Status::cleaning_requested() const noexcept { return gstat_ & GMT_CLN; }

int Status::density_code() const noexcept {
    return static_cast<int>((dsreg_ & MT_ST_DENSITY_MASK) >> MT_ST_DENSITY_SHIFT);
}

int Status::block_size() const noexcept {
    return static_cast<int>((dsreg_ & MT_ST_BLKSIZE_MASK) >> MT_ST_BLKSIZE_SHIFT);
}

std::string_view Status::density_name() const {
    auto *n = density_name_for_code(density_code());
    return n ? std::string_view(n) : std::string_view("unknown");
}

std::string Status::type_name() const {
    if (type_ == MT_ISSCSI1)   return "SCSI 1";
    if (type_ == MT_ISSCSI2)   return "SCSI 2";
    if (type_ == MT_ISONSTREAM_SC) return "OnStream SC-, DI-, DP-, or USB";
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
    : device_path_(device_path), read_write_(read_write)
{
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
    : fd_(fd), device_path_(path), read_write_(read_write)
{
}

TapeDevice::~TapeDevice() {
    if (fd_ >= 0) ::close(fd_);
}

TapeDevice::TapeDevice(TapeDevice &&other) noexcept
    : fd_(other.fd_),
      device_path_(std::move(other.device_path_)),
      read_write_(other.read_write_)
{
    other.fd_ = -1;
}

TapeDevice &TapeDevice::operator=(TapeDevice &&other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        device_path_ = std::move(other.device_path_);
        read_write_ = other.read_write_;
        other.fd_ = -1;
    }
    return *this;
}

// -- low-level ioctl dispatch ------------------------------------------

void TapeDevice::do_mtop(int op, int count) {
    mtop mt_com{};
    mt_com.mt_op   = static_cast<short>(op);
    mt_com.mt_count = count;

    if (::ioctl(fd_, MTIOCTOP, &mt_com) < 0)
        throw Error(device_path_, "ioctl", errno);
}

// -- positioning -------------------------------------------------------

void TapeDevice::rewind()                    { do_mtop(MTREW, 1); }
void TapeDevice::space_to_eod()              { do_mtop(MTEOM, 1); }
void TapeDevice::space_fwd(int count)        { do_mtop(MTFSF, count); }
void TapeDevice::space_bwd(int count)        { do_mtop(MTBSF, count); }
void TapeDevice::space_fwd_filemark(int cnt) { do_mtop(MTFSFM, cnt); }
void TapeDevice::space_bwd_filemark(int cnt) { do_mtop(MTBSFM, cnt); }
void TapeDevice::space_fwd_records(int cnt)  { do_mtop(MTFSR, cnt); }
void TapeDevice::space_bwd_records(int cnt)  { do_mtop(MTBSR, cnt); }
void TapeDevice::space_fwd_setmarks(int cnt) { do_mtop(MTFSS, cnt); }
void TapeDevice::space_bwd_setmarks(int cnt) { do_mtop(MTBSS, cnt); }

void TapeDevice::seek_block(long block_no) {
    do_mtop(MTSEEK, static_cast<int>(block_no));
}

Position TapeDevice::tell() {
    return do_tell();
}

// -- markers -----------------------------------------------------------

void TapeDevice::write_filemark(int count)   { do_mtop(MTWEOF, count); }
void TapeDevice::write_setmark(int count)    { do_mtop(MTWSM, count); }
void TapeDevice::erase(int count)            { do_mtop(MTERASE, count); }

// -- drive control -----------------------------------------------------

void TapeDevice::set_block_size(int bytes)   { do_mtop(MTSETBLK, bytes); }
void TapeDevice::set_density(int code)       { do_mtop(MTSETDENSITY, code); }
void TapeDevice::set_compression(bool en)    { do_mtop(MTCOMPRESSION, en ? 1 : 0); }
void TapeDevice::lock()                      { do_mtop(MTLOCK, 1); }
void TapeDevice::unlock()                    { do_mtop(MTUNLOCK, 1); }
void TapeDevice::load(int count)             { do_mtop(MTLOAD, count); }
void TapeDevice::offline()                   { do_mtop(MTOFFL, 1); }

// -- status queries ----------------------------------------------------

Status TapeDevice::status() {
    return do_status();
}

bool TapeDevice::is_online()           { return status().online(); }
bool TapeDevice::is_write_protected()  { return status().wr_prot(); }
int  TapeDevice::get_fileno()          { return status().fileno(); }
int  TapeDevice::get_blkno()           { return status().blkno(); }

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
    return Status(raw.mt_type, raw.mt_resid, raw.mt_dsreg,
                  raw.mt_gstat, raw.mt_erreg,
                  static_cast<int>(raw.mt_fileno),
                  static_cast<int>(raw.mt_blkno));
}

// -- static helpers ----------------------------------------------------

std::string_view TapeDevice::density_name(int code) {
    auto *n = density_name_for_code(code);
    return n ? std::string_view(n) : std::string_view("unknown");
}

bool TapeDevice::is_scsi_tape(int fd) {
    mtget raw{};
    if (::ioctl(fd, MTIOCGET, &raw) < 0)
        return false;
    return raw.mt_type == MT_ISSCSI1 || raw.mt_type == MT_ISSCSI2;
}

} // namespace mt
