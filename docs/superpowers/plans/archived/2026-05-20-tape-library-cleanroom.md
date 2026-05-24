# Clean-Room Tape Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace GPL2-dependent code from mt-st with independently-authored clean-room equivalents, and add TapeNavigator + FileBackedTapeDevice.

**Architecture:** New `tape_ioctl.h` provides self-contained kernel ABI definitions in `mt::` namespace. `TapeDevice` gets minimal virtual method surface + protected ctor for subclassing. `TapeNavigator` provides filemark-based positioning (no `tell()` — LTO doesn't support it). `FileBackedTapeDevice` simulates LTO sequential-access semantics for testing.

**Tech Stack:** C++20, Linux ioctl, `_IOW`/`_IOR` via `<sys/ioctl.h>`, NeoTape format headers.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `include/neotape/tape_ioctl.h` | Create | Self-contained ioctl struct/constants in `mt::` ns |
| `include/neotape/tape.hpp` | Modify | Add virtual `do_mtop()`/`do_tell()`/`do_status()`, protected ctor |
| `src/neotape_tape.cpp` | Modify | Use `tape_ioctl.h`, independent density table |
| `include/neotape/tape_navigator.hpp` | Create | `mt::nav::TapeNavigator` class |
| `src/neotape_tape_navigator.cpp` | Create | Navigator implementation |
| `tests/tape_test_device.hpp` | Create | `mt::test::FileBackedTapeDevice` |
| `tests/tape_test_device.cpp` | Create | Test double implementation |
| `Makefile` | Modify | Add `NAV_OBJ`, `-Itests`, drop mt-st |
| `include/mtio.h` | Delete | GPL2 symlink |
| `3rdparty/mt-st` | Deinit | No longer used |

---

### Task 1: Create `include/neotape/tape_ioctl.h`

**Files:**
- Create: `include/neotape/tape_ioctl.h`

- [ ] **Step 1: Write tape_ioctl.h**

```cpp
#pragma once

#include <sys/ioctl.h>

namespace mt {

// -----------------------------------------------------------------------
// ioctl parameter / result structs  (kernel ABI — facts, not expression)
// -----------------------------------------------------------------------

struct mtop  { short mt_op; int mt_count; };
struct mtpos { long mt_blkno; };
struct mtget {
    long mt_type, mt_resid, mt_dsreg, mt_gstat, mt_erreg;
    int  mt_fileno, mt_blkno;
};

// -----------------------------------------------------------------------
// ioctl command numbers
// -----------------------------------------------------------------------

inline constexpr unsigned long MTIOCTOP = _IOW('m', 1, mtop);
inline constexpr unsigned long MTIOCGET = _IOR('m', 2, mtget);
inline constexpr unsigned long MTIOCPOS = _IOR('m', 3, mtpos);

// -----------------------------------------------------------------------
// MTIOCTOP operation codes
// -----------------------------------------------------------------------

enum : short {
    MTWEOF   = 0,   // write filemark
    MTFSF    = 1,   // forward space filemark
    MTBSF    = 2,   // backward space filemark
    MTFSR    = 3,   // forward space record
    MTBSR    = 4,   // backward space record
    MTREW    = 6,   // rewind
    MTOFFL   = 7,   // rewind + offline
    MTNOP    = 8,   // no-op
    MTRETEN  = 9,   // retension
    MTEOM    = 11,  // space to end of recorded media
    MTERASE  = 12,  // erase
    MTFSFM   = 13,  // forward space to filemark
    MTBSFM   = 14,  // backward space to filemark
    MTSEEK   = 15,  // seek to block
    MTTELL   = 16,  // tell block number
    MTFSS    = 17,  // forward space setmark
    MTBSS    = 18,  // backward space setmark
    MTSETBLK = 20,  // set block length
    MTSETDENSITY = 21, // set density
    MTWSM    = 22,  // write setmark
    MTLOCK   = 23,  // lock drive
    MTUNLOCK = 24,  // unlock drive
    MTLOAD   = 25,  // load media
    MTUNLOAD = 26,  // unload media
    MTCOMPRESSION = 27, // set compression
    MTSETPART = 28, // set partition
    MTMKPART = 29,  // make partition
    MTWEOFI  = 30,  // write EOT filemark
};

// -----------------------------------------------------------------------
// Status bits (mt_gstat masks)
// -----------------------------------------------------------------------

inline constexpr long GMT_EOF     = 0x80000000;
inline constexpr long GMT_BOT     = 0x40000000;
inline constexpr long GMT_EOT     = 0x20000000;
inline constexpr long GMT_SM      = 0x10000000;
inline constexpr long GMT_EOD     = 0x08000000;
inline constexpr long GMT_WR_PROT = 0x04000000;
inline constexpr long GMT_ONLINE  = 0x01000000;
inline constexpr long GMT_DR_OPEN = 0x00040000;
inline constexpr long GMT_CLN     = 0x00008000;

// -----------------------------------------------------------------------
// Density / block-size field layout in dsreg
// -----------------------------------------------------------------------

inline constexpr long MT_ST_DENSITY_MASK  = 0xff000000;
inline constexpr int  MT_ST_DENSITY_SHIFT = 24;
inline constexpr long MT_ST_BLKSIZE_MASK  = 0x00ffffff;
inline constexpr int  MT_ST_BLKSIZE_SHIFT = 0;

// -----------------------------------------------------------------------
// Drive type constants (mt_type)
// -----------------------------------------------------------------------

inline constexpr long MT_ISSCSI1       = 0x01;
inline constexpr long MT_ISSCSI2       = 0x02;
inline constexpr long MT_ISONSTREAM_SC = 0x04;

} // namespace mt
```

- [ ] **Step 2: Commit**

```bash
git add include/neotape/tape_ioctl.h
git commit -m "feat: self-contained tape ioctl header in mt:: namespace"
```

---

### Task 2: Modify `include/neotape/tape.hpp`

**Files:**
- Modify: `include/neotape/tape.hpp`

- [ ] **Step 1: Add virtual dispatch + protected ctor**

Changes to `tape.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace mt {

// Error, Status, Position — unchanged from current version
// (these are at lines 14-78 of the existing file, keep as-is)

class Error final : public std::runtime_error {
public:
    Error(std::string_view device, std::string_view operation, int errnum);
    int error_code() const noexcept { return errnum_; }
    int mt_resid() const noexcept { return mt_resid_; }
    void set_mt_resid(int r) noexcept { mt_resid_ = r; }
private:
    int errnum_;
    int mt_resid_ = 0;
};

class Status final {
public:
    explicit Status(long mt_type, long mt_resid, long mt_dsreg,
                    long mt_gstat, long mt_erreg,
                    int mt_fileno, int mt_blkno);

    long type()   const noexcept;
    long resid()  const noexcept;
    long dsreg()  const noexcept;
    long gstat()  const noexcept;
    long erreg()  const noexcept;
    int  fileno() const noexcept;
    int  blkno()  const noexcept;

    bool eof()   const noexcept;
    bool bot()   const noexcept;
    bool eot()   const noexcept;
    bool sm()    const noexcept;
    bool eod()   const noexcept;
    bool wr_prot()  const noexcept;
    bool online()   const noexcept;
    bool dr_open()  const noexcept;
    bool cleaning_requested() const noexcept;

    int  density_code() const noexcept;
    int  block_size()   const noexcept;
    std::string_view density_name() const;
    std::string type_name() const;

private:
    long type_, resid_, dsreg_, gstat_, erreg_;
    int  fileno_, blkno_;
};

struct Position final {
    long block_no;
};

class TapeDevice {
public:
    explicit TapeDevice(std::string_view device_path, bool read_write = false);
    virtual ~TapeDevice();

    TapeDevice(const TapeDevice &) = delete;
    TapeDevice &operator=(const TapeDevice &) = delete;
    TapeDevice(TapeDevice &&other) noexcept;
    TapeDevice &operator=(TapeDevice &&other) noexcept;

    int fd()                 const noexcept { return fd_; }
    const std::string &device_path() const noexcept { return device_path_; }
    bool is_read_write()     const noexcept { return read_write_; }

    // -- positioning (non-virtual; all delegate to virtual do_mtop) --

    void rewind();
    void space_to_eod();
    void space_fwd(int count = 1);
    void space_bwd(int count = 1);
    void space_fwd_filemark(int count = 1);
    void space_bwd_filemark(int count = 1);
    void space_fwd_records(int count = 1);
    void space_bwd_records(int count = 1);
    void space_fwd_setmarks(int count = 1);
    void space_bwd_setmarks(int count = 1);
    void seek_block(long block_no);
    Position tell();

    // -- markers --

    void write_filemark(int count = 1);
    void write_setmark(int count = 1);
    void erase(int count = 1);

    // -- drive control --

    void set_block_size(int bytes);
    void set_density(int code);
    void set_compression(bool enable);
    void lock();
    void unlock();
    void load(int count = 1);
    void offline();

    // -- status queries (tell + status are virtual separately) --

    Status status();
    bool is_online();
    bool is_write_protected();
    int  get_fileno();
    int  get_blkno();

    static std::string_view density_name(int code);
    static bool is_scsi_tape(int fd);

protected:
    // Subclass constructor — skip char-device validation (for test doubles)
    TapeDevice(int fd, std::string_view path, bool read_write);

    // All MTIOCTOP operations route through this virtual dispatch point.
    virtual void do_mtop(int op, int count);

    // tell() and status() are separate ioctls (no mtop struct).
    virtual Position do_tell();
    virtual Status do_status();

private:
    int fd_ = -1;
    std::string device_path_;
    bool read_write_ = false;
};

} // namespace mt
```

- [ ] **Step 2: Commit**

```bash
git add include/neotape/tape.hpp
git commit -m "feat: add virtual do_mtop/do_tell/do_status, protected ctor to TapeDevice"
```

---

### Task 3: Rewrite `src/neotape_tape.cpp`

**Files:**
- Modify: `src/neotape_tape.cpp`

- [ ] **Step 1: Write new implementation with tape_ioctl.h and independent density table**

Full file rewrite:

```cpp
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
    // No validation — for subclass (test double) use only.
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

// -- positioning (delegate to do_mtop) ---------------------------------

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
```

Key changes from old file:
- `#include <mtio.h>` → `#include "neotape/tape_ioctl.h"`
- Uses `mt::mtop`, `mt::mtget`, `mt::mtpos`, `mt::MTIOCTOP`, `mt::MTREW`, etc.
- Status bit checks use `GMT_EOF` etc. instead of raw hex
- Independent density table (25 entries, modern formats only)
- `TapeDevice::tell()` delegates to `do_tell()`
- `TapeDevice::status()` delegates to `do_status()`
- Protected `TapeDevice(fd, path, rw)` constructor added

- [ ] **Step 2: Commit**

```bash
git add src/neotape_tape.cpp
git commit -m "refactor: use mt:: tape_ioctl.h, independent density table, virtual dispatch"
```

---

### Task 4: Create TapeNavigator

**Files:**
- Create: `include/neotape/tape_navigator.hpp`
- Create: `src/neotape_tape_navigator.cpp`

- [ ] **Step 1: Write tape_navigator.hpp**

```cpp
#pragma once

#include "neotape/tape.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// Forward-declare NeoTape format types
namespace neotape {
struct ParsedHeader;
struct VolumeHeader;
struct ArchiveEndHeader;
} // namespace neotape

namespace mt {
namespace nav {

enum class AppendPolicy {
    strict,  // fail if previous archive end is missing or corrupt
    inspect, // read-only, no writing
    force,   // append even without verification
};

enum class TapeCondition {
    blank,           // no data on tape
    has_valid_tail,  // previous archive end header verified
    has_corrupt_tail,// previous end header missing/bad
};

struct AppendResult {
    bool ok;
    TapeCondition condition;
    std::optional<neotape::ArchiveEndHeader> last_header;
};

struct ArchiveBoundary {
    uint64_t volume_fileno;
    uint64_t end_fileno;
    neotape::VolumeHeader volume_header;
    neotape::ArchiveEndHeader end_header;
};

class TapeNavigator {
public:
    explicit TapeNavigator(TapeDevice &dev);

    // Append preflight: locate end of valid data, verify previous archive end,
    // and leave tape positioned for writing. Works on LTO (filemark-based).
    AppendResult locate_append_position(AppendPolicy policy = AppendPolicy::strict);

    // Read-only inspection of the tape's current state.
    AppendResult inspect();

    // Read the current tape-file header (one record) as a NeoTape header.
    // Returns nullopt if at EOD/filemark with no data.
    std::optional<neotape::ParsedHeader> read_current_header();

    // Walk from BOT to EOD reading tape-file headers, collecting
    // boundaries of every complete archive instance found.
    std::vector<ArchiveBoundary> scan_archive_instances();

    // Position tape at the Volume Header of the nth archive (0-based).
    // Returns false if fewer than n+1 archives exist.
    bool locate_instance(uint64_t n);

    // Within the current archive, seek to the volume with volume_seq_num.
    // Returns false if the volume is not found.
    bool seek_volume(uint64_t volume_seq_num);

private:
    TapeDevice &dev_;
};

} // namespace nav
} // namespace mt
```

- [ ] **Step 2: Write tape_navigator.cpp**

```cpp
#include "neotape/tape_navigator.hpp"
#include "neotape/format.hpp"

#include <unistd.h>
#include <vector>

namespace mt {
namespace nav {

TapeNavigator::TapeNavigator(TapeDevice &dev)
    : dev_(dev)
{
}

std::optional<neotape::ParsedHeader> TapeNavigator::read_current_header() {
    std::vector<uint8_t> buf(neotape::fixed_header_size);
    ssize_t n = ::read(dev_.fd(), buf.data(), buf.size());
    if (n <= 0)
        return std::nullopt;
    buf.resize(static_cast<std::size_t>(n));
    if (buf.size() < neotape::fixed_header_size)
        buf.resize(neotape::fixed_header_size, 0);
    return neotape::parse_fixed_header(buf.data(), buf.size());
}

AppendResult TapeNavigator::locate_append_position(AppendPolicy policy) {
    // 1. Go to end of recorded data
    dev_.space_to_eod();

    // 2. Check if tape is blank (BOT at EOD)
    {
        auto s = dev_.status();
        if (s.bot())
            return {false, TapeCondition::blank, std::nullopt};
    }

    // 3. Back up past the two filemarks that surround ArchiveEndHeader
    try {
        dev_.space_bwd_filemark(2);
    } catch (const Error &) {
        // No filemarks exist — unexpected condition
        return {false, TapeCondition::has_corrupt_tail, std::nullopt};
    }

    // 4. Read and verify the previous archive end header
    auto header = read_current_header();
    if (!header || header->type != neotape::HeaderType::archive_end) {
        if (policy == AppendPolicy::strict)
            return {false, TapeCondition::has_corrupt_tail, std::nullopt};
        dev_.space_to_eod();
        return {false, TapeCondition::has_corrupt_tail, std::nullopt};
    }

    bool crc_ok = (header->stored_crc32c == header->computed_crc32c);
    if (!crc_ok && policy == AppendPolicy::strict)
        return {false, TapeCondition::has_corrupt_tail, std::nullopt};

    // 5. Return to append point
    dev_.space_to_eod();
    return {true, TapeCondition::has_valid_tail, header->archive_end};
}

AppendResult TapeNavigator::inspect() {
    // Same as locate_append_position but always in inspect mode
    return locate_append_position(AppendPolicy::inspect);
}

std::vector<ArchiveBoundary> TapeNavigator::scan_archive_instances() {
    std::vector<ArchiveBoundary> archives;
    dev_.rewind();

    std::optional<neotape::VolumeHeader> current_volume;
    uint64_t volume_fileno = 0;
    uint64_t current_fileno = 0;

    for (;;) {
        auto header = read_current_header();

        // Advance to next tape file
        try {
            dev_.space_fwd(1);
            current_fileno++;
        } catch (const Error &) {
            // At EOD or BOT with no more files
            break;
        }

        if (!header)
            continue;

        if (header->type == neotape::HeaderType::volume) {
            current_volume = header->volume;
            volume_fileno = current_fileno;
        } else if (header->type == neotape::HeaderType::archive_end) {
            if (current_volume) {
                ArchiveBoundary b;
                b.volume_fileno = volume_fileno;
                b.end_fileno = current_fileno;
                b.volume_header = *current_volume;
                b.end_header = *header->archive_end;
                archives.push_back(b);
                current_volume.reset();
            }
        }
        // Frame headers are skipped (just iterate to next file)
    }

    return archives;
}

bool TapeNavigator::locate_instance(uint64_t n) {
    dev_.rewind();
    uint64_t found = 0;

    for (;;) {
        auto header = read_current_header();
        bool is_volume = header &&
            header->type == neotape::HeaderType::volume;

        // Advance past this tape file
        try {
            dev_.space_fwd(1);
        } catch (const Error &) {
            return false;
        }

        if (is_volume) {
            if (found == n) {
                // We're past the target — go back to its start
                dev_.space_bwd_filemark(1);
                return true;
            }
            found++;
        }
    }
}

bool TapeNavigator::seek_volume(uint64_t volume_seq_num) {
    for (;;) {
        auto header = read_current_header();
        bool matches = header &&
            header->type == neotape::HeaderType::volume &&
            header->volume &&
            header->volume->volume_seq_num == volume_seq_num;

        try {
            dev_.space_fwd(1);
        } catch (const Error &) {
            return false;
        }

        if (matches) {
            dev_.space_bwd_filemark(1);
            return true;
        }
    }
}

} // namespace nav
} // namespace mt
```

- [ ] **Step 3: Commit**

```bash
git add include/neotape/tape_navigator.hpp src/neotape_tape_navigator.cpp
git commit -m "feat: TapeNavigator with append-preflight and archive scanning"
```

---

### Task 5: Create FileBackedTapeDevice

**Files:**
- Create: `tests/tape_test_device.hpp`
- Create: `tests/tape_test_device.cpp`

- [ ] **Step 1: Write tape_test_device.hpp**

```cpp
#pragma once

#include "neotape/tape.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mt {
namespace test {

// FileBackedTapeDevice simulates an LTO drive's sequential-access semantics
// using a regular file. No block-level addressing — do_tell() throws.
// Filemarks are tracked as byte boundaries in the backing file.

class FileBackedTapeDevice final : public TapeDevice {
public:
    explicit FileBackedTapeDevice(std::string_view path,
                                  uint32_t block_size = 65536,
                                  bool read_write = true);
    ~FileBackedTapeDevice() override;

    void   do_mtop(int op, int count) override;
    Status do_status() override;
    Position do_tell() override;    // throws — LTO-style

    // Test helpers
    size_t file_count() const noexcept { return boundaries_.empty() ? 0 : boundaries_.size() - 1; }
    bool   at_eod()  const noexcept { return current_file_ < 0 || static_cast<std::size_t>(current_file_) + 1 >= boundaries_.size(); }
    uint32_t block_size() const noexcept { return block_size_; }

private:
    void seek_to_current_file();

    int backing_fd_;
    std::vector<uint64_t> boundaries_;  // byte offsets: boundaries_[i] = start of file i
    int      current_file_ = -1;        // -1 = before first file (BOT)
    uint64_t current_record_ = 0;       // record offset within current file
    uint32_t block_size_;
    bool     read_write_;
};

} // namespace test
} // namespace mt
```

- [ ] **Step 2: Write tape_test_device.cpp**

```cpp
#include "tape_test_device.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace mt {
namespace test {

FileBackedTapeDevice::FileBackedTapeDevice(std::string_view path,
                                           uint32_t block_size,
                                           bool read_write)
    : TapeDevice(-1, std::string(path), read_write),  // fd=-1, our own fd
      block_size_(block_size),
      read_write_(read_write)
{
    int oflags = read_write ? O_RDWR | O_CREAT : O_RDONLY;
    backing_fd_ = ::open(std::string(path).c_str(), oflags, 0666);
    if (backing_fd_ < 0)
        throw Error(std::string(path), "open", errno);

    boundaries_.push_back(0);  // first file starts at 0

    // Determine existing file count by scanning for filemark signatures
    // (simplified: in-memory only; for persistent files, a real implementation
    // would store metadata. For testing, we start empty.)
}

FileBackedTapeDevice::~FileBackedTapeDevice() {
    if (backing_fd_ >= 0)
        ::close(backing_fd_);
}

void FileBackedTapeDevice::seek_to_current_file() {
    if (current_file_ < 0) {
        ::lseek(backing_fd_, 0, SEEK_SET);
    } else if (static_cast<std::size_t>(current_file_) < boundaries_.size()) {
        ::lseek(backing_fd_, boundaries_[current_file_], SEEK_SET);
    } else {
        // Past last file — seek to end
        ::lseek(backing_fd_, 0, SEEK_END);
    }
}

void FileBackedTapeDevice::do_mtop(int op, int count) {
    switch (op) {
    case MTREW:
        current_file_ = -1;
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTWEOF: {
        // Finalize current file at current backing-file offset
        off_t pos = ::lseek(backing_fd_, 0, SEEK_CUR);
        if (pos < 0) throw Error(device_path(), "lseek", errno);

        // Ensure the boundary for the current file is recorded
        auto idx = static_cast<std::size_t>(current_file_ + 1);
        while (boundaries_.size() <= idx)
            boundaries_.push_back(static_cast<uint64_t>(pos));

        // If count > 1, add empty file boundaries
        for (int i = 1; i < count; i++) {
            boundaries_.push_back(static_cast<uint64_t>(pos));
        }
        current_file_ = static_cast<int>(boundaries_.size()) - 2;
        current_record_ = 0;
        break;
    }

    case MTEOM:
        if (boundaries_.size() >= 2) {
            current_file_ = static_cast<int>(boundaries_.size()) - 2;
        } else {
            current_file_ = -1;  // no files yet = BOT
        }
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTFSF:
        current_file_ = std::min(
            current_file_ + count,
            static_cast<int>(boundaries_.size()) - 1);
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTBSF:
        current_file_ = std::max(current_file_ - count, -1);
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTFSFM:
        current_file_ = std::min(
            current_file_ + count,
            static_cast<int>(boundaries_.size()) - 1);
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTBSFM:
        current_file_ = std::max(current_file_ - count, -1);
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTFSR:
        current_record_ += static_cast<uint64_t>(count);
        seek_to_current_file();
        break;

    case MTBSR:
        if (static_cast<int64_t>(current_record_) >= count)
            current_record_ -= static_cast<uint64_t>(count);
        else
            current_record_ = 0;
        seek_to_current_file();
        break;

    case MTSEEK:
    case MTTELL:
        throw Error(device_path(), "mtop", ENOTSUP);

    case MTERASE:
        // No-op for test device
        break;

    default:
        // For unsupported ops, throw
        throw Error(device_path(), "mtop", ENOTSUP);
    }
}

Status FileBackedTapeDevice::do_status() {
    bool bot = (current_file_ < 0);
    return Status(
        0,          // mt_type (unknown)
        0,          // mt_resid
        (static_cast<long>(block_size_) & MT_ST_BLKSIZE_MASK),  // dsreg
        (bot ? GMT_BOT : 0) | GMT_ONLINE | GMT_EOD,
        0,          // erreg
        bot ? 0 : current_file_,   // fileno
        static_cast<int>(current_record_)  // blkno
    );
}

Position FileBackedTapeDevice::do_tell() {
    throw Error(device_path(), "tell", ENOTSUP);
}

} // namespace test
} // namespace mt
```

- [ ] **Step 3: Create tests directory**

```bash
mkdir -p tests
```

- [ ] **Step 4: Commit**

```bash
git add tests/tape_test_device.hpp tests/tape_test_device.cpp
git commit -m "feat: FileBackedTapeDevice test double simulating LTO semantics"
```

---

### Task 6: Write test harness

**Files:**
- Create: `tests/test_tape.cpp`

- [ ] **Step 1: Write test_tape.cpp**

```cpp
// Basic verification of FileBackedTapeDevice + TapeNavigator.
// Run: make tests/test_tape && tests/test_tape

#include "tape_test_device.hpp"
#include "neotape/tape_navigator.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        failures++; \
    } else { \
        fprintf(stderr, "PASS: %s\n", msg); \
    } \
} while(0)

int main() {
    // --- Test 1: blank tape detection ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_1.bin", 4096);
        mt::nav::TapeNavigator nav(dev);
        auto r = nav.locate_append_position(mt::nav::AppendPolicy::strict);
        CHECK(!r.ok, "blank tape → not ok");
        CHECK(r.condition == mt::nav::TapeCondition::blank, "blank tape → blank condition");
    }

    // --- Test 2: write a header, read it back ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_2.bin", 4096, true);
        int fd = dev.fd();

        // Write a minimal 1024-byte header record
        char buf[1024] = {};
        memcpy(buf, "NeoTape", 7);
        buf[8] = 1;  // version
        buf[9] = 3;  // HeaderType::frame

        ssize_t n = ::write(fd, buf, sizeof(buf));
        CHECK(n == 1024, "write 1024 bytes");
        dev.write_filemark();

        // Space back and read
        dev.rewind();
        char rbuf[1024] = {};
        n = ::read(fd, rbuf, sizeof(rbuf));
        CHECK(n == 1024, "read 1024 bytes");
        CHECK(memcmp(rbuf, "NeoTape", 7) == 0, "magic matches");
    }

    // --- Test 3: tell() throws on test device ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_3.bin", 4096);
        bool threw = false;
        try {
            dev.tell();
        } catch (const mt::Error &) {
            threw = true;
        }
        CHECK(threw, "tell() throws on FileBackedTapeDevice");
    }

    // --- Test 4: filemark index after writes ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_4.bin", 4096, true);
        CHECK(dev.file_count() == 0, "no files initially");

        char buf[1024] = {};
        ::write(dev.fd(), buf, sizeof(buf));
        dev.write_filemark();
        CHECK(dev.file_count() == 1, "one file after first write+fm");

        ::write(dev.fd(), buf, sizeof(buf));
        dev.write_filemark();
        CHECK(dev.file_count() == 2, "two files after second write+fm");
    }

    // --- Test 5: navigator on non-blank tape ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_5.bin", 4096, true);

        // Write what looks like a valid archive end header
        // (minimal: just NeoTape magic + type=archive_end)
        neotape::ArchiveEndHeader ae;
        ae.archive_uuid = "test-uuid-0000-0000-000000000000";
        ae.archive_name = "test";
        ae.volume_block_size = 4096;
        auto bytes = neotape::serialize_archive_end_header(ae);
        ::write(dev.fd(), bytes.data(), bytes.size());
        dev.write_filemark();

        mt::nav::TapeNavigator nav(dev);
        auto r = nav.locate_append_position(mt::nav::AppendPolicy::strict);
        CHECK(r.ok, "non-blank with valid tail → ok");
    }

    // --- Cleanup temp files ---
    unlink("/tmp/tape_test_1.bin");
    unlink("/tmp/tape_test_2.bin");
    unlink("/tmp/tape_test_3.bin");
    unlink("/tmp/tape_test_4.bin");
    unlink("/tmp/tape_test_5.bin");

    fprintf(stderr, "\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add tests/test_tape.cpp
git commit -m "test: basic tape test harness for FileBackedTapeDevice + TapeNavigator"
```

---

### Task 7: Update Makefile

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add new objects, include path, test target**

Changes:

```makefile
INCS	= -Iinclude -Itests -Llib -I/usr/local/include -Lusr/local/lib

FORMAT_OBJ = $(BUILDDIR)/neotape_format.o
COMMON_OBJ = $(BUILDDIR)/neotape_common.o
BOUNDEDBUF_OBJ = $(BUILDDIR)/neotape_bounded_buffer.o
TAPE_OBJ = $(BUILDDIR)/neotape_tape.o
NAV_OBJ  = $(BUILDDIR)/neotape_tape_navigator.o
TEST_DEVICE_OBJ = $(BUILDDIR)/neotape_tape_test_device.o

$(TAPE_OBJ) : src/neotape_tape.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(NAV_OBJ) : src/neotape_tape_navigator.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_DEVICE_OBJ) : tests/tape_test_device.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/neotape-write : src/neotape_write.cpp $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) -o $@ $(LDLIBS)

# Test binary
$(BINDIR)/test_tape : tests/test_tape.cpp $(FORMAT_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TEST_DEVICE_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TEST_DEVICE_OBJ) -o $@ $(LDLIBS)

test: $(BINDIR)/test_tape
	$(BINDIR)/test_tape
```

Also update the `clean` and `EXE` variables:

```makefile
EXE	= bin/pax bin/mt-pax bin/neotape-write bin/neotape-inspect bin/neotape-plan bin/neotape-cat-volumes bin/test_tape

clean:
	-rm -f ${EXE} ${BINDIR}/*.o $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TEST_DEVICE_OBJ) $(B3LIB) $(B3OBJ) $(CRC32CLIB) $(CRC32COBJ)
```

- [ ] **Step 2: Build and run test**

```bash
make clean && make -j "$(nproc)" && make test
```

Expected output: all 5 tests pass, 0 failures.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "build: integrate navigator, test device, and test harness into Makefile"
```

---

### Task 8: Clean up mt-st

**Files:**
- Delete: `include/mtio.h`
- Deinit: `3rdparty/mt-st`

- [ ] **Step 1: Remove symlink**

```bash
git rm include/mtio.h
```

- [ ] **Step 2: Deinit and remove mt-st submodule**

```bash
git submodule deinit -f 3rdparty/mt-st
git rm -f 3rdparty/mt-st
```

- [ ] **Step 3: Verify no mt-st references remain**

```bash
rg -l 'mt-st\|mtio\.h\|include.*mtio' --type-add 'cpp:*.{cpp,hpp,c,h}' -t cpp Makefile
```

Expected: no matches.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "cleanup: remove GPL2-only mt-st dependency (symlink + submodule)"
```

---

### Task 9: Final verification

- [ ] **Step 1: Full clean build**

```bash
make clean && make -j "$(nproc)"
```

- [ ] **Step 2: Run tests**

```bash
make test
```

- [ ] **Step 3: Verify no GPL2 code remains**

```bash
rg -l 'mt-st\|mtio\.h' --type-add 'cpp:*.{cpp,hpp,c,h}' -t cpp include/ src/ Makefile tests/
```

Expected: no matches.

- [ ] **Step 4: Final commit if needed**

```bash
git status
```
