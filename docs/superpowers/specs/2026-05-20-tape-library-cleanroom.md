# Clean-Room Tape Operation Library

Date: 2026-05-20
Status: Spec

## Problem

The file `include/mtio.h` is a symlink to `3rdparty/mt-st/mtio.h`. The
density table in `src/neotape_tape.cpp` was copied from `mt-st/mt.c`.
Both originate from the `mt-st` project, which is GPL2-only. The NeoTape
project is GPL3. GPL2-only and GPL3 are incompatible.

The `3rdparty/mt-st` submodule tracks upstream at v1.8. Only `mtio.h` is
consumed by NeoTape (via the symlink); the `mt.c` and `stinit.c` sources
are not built.

## Solution

Replace the GPL2-dependent components with independently-authored clean-room
equivalents, and add higher-level tape navigation and test infrastructure.

### Files to create

| File | Purpose |
|------|---------|
| `include/neotape/tape_ioctl.h` | Self-contained ioctl struct/constant definitions in `mt::` namespace |
| `include/neotape/tape_navigator.hpp` | `mt::nav::TapeNavigator` class |
| `src/neotape_tape_navigator.cpp` | Navigator implementation |
| `tests/tape_test_device.hpp` | `mt::test::FileBackedTapeDevice` header |
| `tests/tape_test_device.cpp` | Test double implementation |

### Files to modify

| File | Change |
|------|--------|
| `include/neotape/tape.hpp` | Virtual destructor, virtual `do_mtop()`/`tell()`/`status()`; protected ctor for subclass use; remove `density_name()` static |
| `src/neotape_tape.cpp` | `#include "neotape/tape_ioctl.h"`; independent minimal density table; `mt::`-qualified names |
| `Makefile` | Add `NAV_OBJ`, `-Itests`, drop mt-st references |

### Files to remove

| File | Reason |
|------|--------|
| `include/mtio.h` | GPL2 symlink, replaced by `tape_ioctl.h` |
| `3rdparty/mt-st` | Entire submodule no longer consumed |

---

## Detailed Design

### 1. `include/neotape/tape_ioctl.h`

Namespace `mt`. Defines only the ioctl structs, operation codes, and status
masks used by the existing `TapeDevice`. These are Linux kernel ABI facts
(integers + struct layouts), not copyrightable expression.

**Structs** (matching the stable `linux/mtio.h` ABI):

```cpp
struct mtop  { short mt_op; int mt_count; };
struct mtpos { long mt_blkno; };
struct mtget { long mt_type, mt_resid, mt_dsreg, mt_gstat, mt_erreg;
               int mt_fileno, mt_blkno; };
```

**Ioctl command numbers** (via `_IOW`/`_IOR` from `<sys/ioctl.h>`):

- `MTIOCTOP` = `_IOW('m', 1, mtop)`
- `MTIOCGET` = `_IOR('m', 2, mtget)`
- `MTIOCPOS` = `_IOR('m', 3, mtpos)`

**Operation codes** (enum, values from kernel `linux/mtio.h`):

`MTWEOF=0, MTFSF=1, MTBSF=2, MTFSR=3, MTBSR=4, MTWEOF=0, MTFSF=1, ...`
— all ~25 ops used in tape.cpp.

**Status masks** (constexpr hex values, already inlined in tape.cpp:157-165):

`GMT_EOF=0x80000000, GMT_BOT=0x40000000, GMT_EOT=0x20000000, ...`

**Density/block-size constants:**

```
MT_ST_DENSITY_MASK   = 0xff000000
MT_ST_DENSITY_SHIFT  = 24
MT_ST_BLKSIZE_MASK   = 0x00ffffff
```

**Drive type constants:**

`MT_ISSCSI1=0x1, MT_ISSCSI2=0x2, MT_ISONSTREAM_SC=0x4`

### 2. Minimal density table (in `src/neotape_tape.cpp`)

Independently researched from public LTO Consortium and IBM specifications.
~20 entries covering only modern formats relevant to the project:

| Code | Description |
|------|-------------|
| 0x40 | LTO-1 Ultrium |
| 0x41 | LTO-2 Ultrium |
| 0x42 | LTO-3 Ultrium |
| 0x44 | LTO-4 Ultrium |
| 0x46 | LTO-5 Ultrium |
| 0x47 | DDS-5 / DAT-72 |
| 0x48 | SDLT-220 |
| 0x49 | SDLT-320 |
| 0x4a | SDLT-600 / T10000A |
| 0x4b | T10000B |
| 0x4c | T10000C |
| 0x4d | T10000D |
| 0x51 | IBM 3592 J1A |
| 0x52 | IBM 3592 E05 (TS1120) |
| 0x53 | IBM 3592 E06 (TS1130) |
| 0x54 | IBM 3592 E07 (TS1140) |
| 0x55 | IBM 3592 E08 (TS1150) |
| 0x56 | IBM 3592 55F (TS1155) |
| 0x57 | IBM 3592 60F (TS1160) |
| 0x58 | LTO-6 Ultrium |
| 0x59 | IBM 3592 70F (TS1170) |
| 0x5a | LTO-7 Ultrium |
| 0x5c | LTO-8 Ultrium |
| 0x5d | LTO-7 M8 |
| 0x5e | LTO-9 Ultrium |

Stored as a `constexpr` array with linear scan, same structure as before
but with independently verified content.

`density_name()` static helper on `TapeDevice` is kept but populates from
this new table. Unrecognized codes return `"unknown"`.

### 3. `TapeDevice` changes

Most methods delegate to `do_mtop()`, the single virtual dispatch point.
`tell()` and `status()` are independent (no `mtop` struct) so they have
their own virtual methods.

**Important:** LTO drives do *not* support `MTIOCPOS` (tell) or `MTSEEK`
(block-level seek) — these are sequential-access devices. `tell()` and
`seek_block()` are provided for backward compatibility with older SCSI-2
drives and virtual tape libraries. On LTO hardware they throw.

However, `MTIOCGET` (status) works on LTO drives and reports the current
tape file number via `mt_fileno` in the `mtget` struct. The navigator and
all high-level code should rely on `status().fileno()` for position
tracking, never on `tell()`.

```cpp
class TapeDevice {
public:
    explicit TapeDevice(std::string_view device_path, bool read_write = false);
    virtual ~TapeDevice();

    int fd() const noexcept;
    const std::string &device_path() const noexcept;
    bool is_read_write() const noexcept;

    // Positioning — all delegate to virtual do_mtop()
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
    void seek_block(long block_no);              // throws on LTO
    Position tell();                              // throws on LTO

    void write_filemark(int count = 1);
    void write_setmark(int count = 1);
    void erase(int count = 1);

    void set_block_size(int bytes);
    void set_density(int code);
    void set_compression(bool enable);
    void lock();
    void unlock();
    void load(int count = 1);
    void offline();

    Status status();
    bool is_online();
    bool is_write_protected();
    int get_fileno();
    int get_blkno();

    static std::string_view density_name(int code);
    static bool is_scsi_tape(int fd);

protected:
    TapeDevice(int fd, std::string_view path, bool read_write);

    // All MTIOCTOP operations route through this single virtual dispatch.
    virtual void do_mtop(int op, int count);

    // Independent ioctls (no mtop struct), virtual for test double use.
    virtual Position do_tell();
    virtual Status do_status();

private:
    int fd_ = -1;
    std::string device_path_;
    bool read_write_ = false;
};
```

### 4. `TapeNavigator`

Namespace `mt::nav`. Location: `include/neotape/tape_navigator.hpp`,
`src/neotape_tape_navigator.cpp`.

```cpp
namespace mt::nav {

enum class AppendPolicy { strict, inspect, force };
enum class TapeCondition {
    blank,
    has_valid_tail,
    has_corrupt_tail,
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

    // Append preflight: EOD → BSFM 2 → verify → EOD
    AppendResult locate_append_position(AppendPolicy policy = AppendPolicy::strict);

    // Read-only inspection of the previous archive tail
    AppendResult inspect();

    // Read the current tape-file header as a NeoTape fixed-size header
    std::optional<neotape::ParsedHeader> read_current_header();

    // Walk from BOT forward, read headers, collect complete archives
    std::vector<ArchiveBoundary> scan_archive_instances();

    // Position at the Volume Header of the nth archive (0-based)
    bool locate_instance(uint64_t n);

    // Within current archive, seek to volume_seq_num
    bool seek_volume(uint64_t volume_seq_num);

private:
    TapeDevice &dev_;
};

} // namespace mt::nav
```

**`locate_append_position` algorithm:**

Uses filemark positioning and `status().fileno()` — no `tell()`/`seek_block()`
— so this works on LTO drives.

```
1. space_to_eod()                      // go to end of data
2. auto s = status();
3. if (s.bot()) → condition=blank      // EOD at BOT = blank tape
4. space_bwd_filemark(2)               // back up past two filemarks
5.                                   // (minimal tape positions at BOT)
6. Read one record (volume_block_size bytes)
7. Parse as NeoTape fixed header
8. Validate: magic, CRC32C, type == archive_end
   - valid → condition = has_valid_tail, save header
   - invalid → condition = has_corrupt_tail
9. space_to_eod()                      // back to append point
```

### 5. `FileBackedTapeDevice`

Namespace `mt::test`. Location: `tests/tape_test_device.hpp`,
`tests/tape_test_device.cpp`.

Models LTO drive behavior: sequential-access, filemark-based navigation,
no block-level addressing. Uses a regular file for data storage.

**Layout:** The backing file stores data sequentially. A filemark index
tracks byte offsets separating each "tape file":

```
backing file: [tape file 0 data][tape file 1 data]...
                         ↑ boundary_0   ↑ boundary_1
```

Each tape file contains one or more fixed-size records
(`volume_block_size` bytes per record, matching the NeoTape block size).

Position is represented as:
- `current_file_`: index into `boundaries_` (−1 = BOT, N = EOD/past-last-file)
- `current_record_`: record offset within the current file (0 = first record)

```cpp
namespace mt::test {

class FileBackedTapeDevice final : public TapeDevice {
public:
    explicit FileBackedTapeDevice(std::string_view path,
                                  uint32_t block_size = 65536,
                                  bool read_write = true);
    ~FileBackedTapeDevice() override;

    // Intercept
    void   do_mtop(int op, int count) override;
    Status do_status() override;
    Position do_tell() override;          // throws — LTO does not support this

    // Test helpers
    size_t file_count() const noexcept;   // number of tape files written
    bool   at_eod()  const noexcept;
    uint32_t block_size() const noexcept;

private:
    int backing_fd_;                       // backing file descriptor
    
    // Byte offsets where each tape file starts in the backing file.
    // boundaries_.size() = N+1 for N files (last entry = total data size).
    std::vector<uint64_t> boundaries_;

    // Current position
    int      current_file_ = -1;           // -1 = BOT, N = EOD
    uint64_t current_record_ = 0;          // record within current file

    uint32_t block_size_;
    bool     read_write_;
};

} // namespace mt::test
```

**Semantics:**

- `do_mtop(MTREW, 1)`: `current_file_ = -1; current_record_ = 0;`
  `lseek(backing_fd_, 0, SEEK_SET)`.
- `do_mtop(MTWEOF, n)`: finalize current file at current byte position,
  append `n` empty file boundaries (zero-byte files if no data was written).
  Advance `current_file_` past them.
- `do_mtop(MTEOM, 1)`: `current_file_ = boundaries_.size() - 1` (last file);
  `lseek(backing_fd_, boundaries_.back(), SEEK_SET)`.
- `do_mtop(MTFSF, count)`: advance `current_file_` by `count` (clamped to
  file count); reposition backing fd via `lseek`.
- `do_mtop(MTBSF, count)`: move backward by `count` files (clamped to −1).
- `do_mtop(MTFSFM, count)`: advance to just past the filemark (equivalent
  to being at the start of the next file after the filemark).
- `do_mtop(MTBSFM, count)`: move backward to just before a filemark.
- `do_mtop(MTSEEK, ...)`: **throws** — LTO drives do not support
  block-level seeking.
- `do_status()`: returns `Status` with `online=true`,
  `wr_prot = !read_write_`, `fileno = max(0, current_file_)`,
  `blkno = current_record_`, `eot=false`, `bot=(current_file_ == -1)`.
- `do_tell()`: **throws** `mt::Error` — not supported on LTO drives.

Callers read/write data via `fd()`, which returns `backing_fd_`. Each
`::read(fd, buf, sz)` or `::write(fd, buf, sz)` operates at the current
backing-file offset, which `do_mtop()` maintains via `lseek`. This
transparently matches LTO record-at-a-time semantics.

---

## Build Integration

Add to `Makefile`:

```makefile
NAV_OBJ    = $(BUILDDIR)/neotape_tape_navigator.o
TEST_DEV_OBJ = $(BUILDDIR)/neotape_tape_test_device.o

INCS += -Itests

$(BINDIR)/neotape-write : ... $(NAV_OBJ) ...
```

The `tape_test_device` object is built only when test binaries are linked.

---

## Removal of mt-st

1. `git rm include/mtio.h` — remove GPL2 symlink
2. `git submodule deinit 3rdparty/mt-st`
3. `git rm 3rdparty/mt-st` — remove submodule

---

## Verification

1. `make clean && make -j "$(nproc)"` succeeds with no mt-st references
2. Existing `TapeDevice` tests (if any) pass
3. `FileBackedTapeDevice` can be constructed and performs positioning
4. `TapeNavigator` works against `FileBackedTapeDevice`
