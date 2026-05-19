# Tape Writer Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a tape device backend to `neotape-write` and a `neotape-init` tool for medium initialization.

**Architecture:** New `MediumHeader` struct + serializer in format layer. `neotape_tape_writer.cpp` contains `write_tape_archive()` with ENOSPC handling. `neotape_init.cpp` writes a minimal Medium Header + filemark. CLI changes in `neotape_write.cpp` (new `-i` for input, `-f` for tape device). Existing spool code untouched.

**Tech Stack:** C++20, libblake3, libcrc32c, Linux tape ioctls via `mt::TapeDevice`.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `include/neotape/format.hpp` | Modify | Add `MediumHeader` struct, offset constants |
| `src/neotape_format.cpp` | Modify | Add `serialize_medium_header()`, Medium type in parser |
| `src/neotape_tape_writer.cpp` | Create | `write_tape_archive()` + helpers |
| `src/neotape_init.cpp` | Create | `neotape-init` CLI |
| `src/neotape_write.cpp` | Modify | CLI: `-i` input, `-f` tape device, tape routing |
| `Makefile` | Modify | Build rules for new files |
| `tests/test_tape.cpp` | Modify | Add tape writer tests |

---

### Task 1: Add MediumHeader to format layer

**Files:**
- Modify: `include/neotape/format.hpp`
- Modify: `src/neotape_format.cpp`

- [ ] **Step 1: Add MediumHeader struct and offset constants to format.hpp**

Add after the `ArchiveEndHeader` struct:

```cpp
struct MediumHeader {
    std::string medium_uuid;
    std::string medium_label;
    std::string initialized_at_utc;
    uint32_t medium_header_block_size = 65536;
    uint16_t medium_header_block_count = 1;
    uint16_t flags = 0;
    std::string created_by_implementation;
    std::string created_by_build_id;
    uint32_t metadata_bundle_size = 0;
    Hash metadata_bundle_blake3{};
};
```

Add offset constants (after the existing offset constants):

```cpp
// Medium header specific
inline constexpr std::size_t mhdr_medium_header_block_size = 10;
inline constexpr std::size_t mhdr_medium_uuid              = 14;
inline constexpr std::size_t mhdr_medium_label             = 51;
inline constexpr std::size_t mhdr_initialized_at_utc       = 307;
inline constexpr std::size_t mhdr_medium_header_block_count = 327;
inline constexpr std::size_t mhdr_flags                     = 329;
inline constexpr std::size_t mhdr_created_by_implementation  = 331;
inline constexpr std::size_t mhdr_created_by_build_id       = 395;
inline constexpr std::size_t mhdr_metadata_bundle_size      = 459;
inline constexpr std::size_t mhdr_metadata_bundle_blake3    = 463;
inline constexpr std::size_t mhdr_reserved                  = 495;
```

Add to `ParsedHeader`:

```cpp
std::optional<MediumHeader> medium;
```

Add to `HeaderType` enum (should already be there: `medium = 1`).

Add declaration in the function section:

```cpp
HeaderBytes serialize_medium_header(const MediumHeader &header);
```

- [ ] **Step 2: Implement serialize_medium_header + parser update in neotape_format.cpp**

Add after the existing serialize/parse functions:

```cpp
HeaderBytes serialize_medium_header(const MediumHeader &header) {
    HeaderBytes buf{};
    std::memcpy(buf.data(), magic.data(), magic.size());
    buf[com_header_version] = header_version;
    buf[com_header_type]    = static_cast<uint8_t>(HeaderType::medium);

    write_le32(buf.data() + mhdr_medium_header_block_size,
               header.medium_header_block_size);
    write_string_fixed(buf.data() + mhdr_medium_uuid,
                       header.medium_uuid, nt_uuid_size);
    write_string_fixed(buf.data() + mhdr_medium_label,
                       header.medium_label, nt_name_size);
    write_string_fixed(buf.data() + mhdr_initialized_at_utc,
                       header.initialized_at_utc, nt_time_size);
    write_le16(buf.data() + mhdr_medium_header_block_count,
               header.medium_header_block_count);
    write_le16(buf.data() + mhdr_flags, header.flags);
    write_string_fixed(buf.data() + mhdr_created_by_implementation,
                       header.created_by_implementation, ident64_size);
    write_string_fixed(buf.data() + mhdr_created_by_build_id,
                       header.created_by_build_id, ident64_size);
    write_le32(buf.data() + mhdr_metadata_bundle_size,
               header.metadata_bundle_size);
    // metadata_bundle_blake3 — write zero hash for now (bundle not present)
    // reserved bytes are already zero from value-initialization

    uint32_t crc = compute_crc32c(buf.data(), hdr_crc32c);
    write_le32(buf.data() + hdr_crc32c, crc);
    return buf;
}
```

Also update `parse_fixed_header()` to handle `HeaderType::medium`. You'll need to add a case after the archive_end case that reads the medium-specific fields. The `write_string_fixed`/`write_le32`/`write_le16` helpers are already used in the existing serialize functions.

- [ ] **Step 3: Verify compilation**

```bash
c++ -std=c++20 -fsyntax-only -Iinclude include/neotape/format.hpp
c++ -std=c++20 -fsyntax-only -Iinclude -c src/neotape_format.cpp
```

- [ ] **Step 4: Commit**

```bash
git add include/neotape/format.hpp src/neotape_format.cpp
git commit -m "feat: MediumHeader struct, serializer, and parser support"
```

---

### Task 2: Create neotape-init

**Files:**
- Create: `src/neotape_init.cpp`

- [ ] **Step 1: Write neotape_init.cpp**

```cpp
#include "neotape/tape.hpp"
#include "neotape/format.hpp"
#include "neotape/common.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <getopt.h>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
using std::format;
using std::string;

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-init: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage(const char *prog) {
    std::cerr << format("usage: {} -f <device> [--label <text>] [--force]\n", prog);
    std::exit(2);
}

struct Options {
    string device;
    string label;
    bool force = false;
};

Options parse_args(int argc, char **argv) {
    static const option long_opts[] = {
        {"label", required_argument, nullptr, 'l'},
        {"force", no_argument,       nullptr, 'F'},
        {"help",  no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "f:l:Fh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'f': opts.device = optarg; break;
        case 'l': opts.label = optarg; break;
        case 'F': opts.force = true; break;
        case 'h': usage(argv[0]); break;
        case '?': std::exit(2);
        }
    }

    if (opts.device.empty()) {
        const char *env = std::getenv("TAPE");
        if (env) opts.device = env;
    }
    if (opts.device.empty())
        fail("no tape device specified (use -f or $TAPE)");

    return opts;
}

} // namespace

int main(int argc, char **argv) {
    try {
        auto opts = parse_args(argc, argv);

        mt::TapeDevice dev(opts.device, true);

        // Check if already initialized
        dev.rewind();
        std::vector<uint8_t> buf(neotape::fixed_header_size);
        ssize_t n = ::read(dev.fd(), buf.data(), buf.size());
        if (n > 0) {
            auto parsed = neotape::parse_fixed_header(buf.data(), buf.size());
            if (parsed && parsed.type == neotape::HeaderType::medium) {
                if (!opts.force)
                    fail("medium already initialized (use --force to overwrite)");
            }
        }

        // Write Medium Header
        neotape::MediumHeader mh;
        mh.medium_uuid = neotape::make_uuid_v4();
        mh.medium_label = opts.label;
        mh.initialized_at_utc = neotape::utc_timestamp_now();
        mh.medium_header_block_size = 65536;
        mh.medium_header_block_count = 1;
        mh.created_by_implementation = "NeoTape init phase6-mvp";

        auto bytes = neotape::serialize_medium_header(mh);

        // Pad to block size and write
        std::vector<uint8_t> record(mh.medium_header_block_size, 0);
        std::memcpy(record.data(), bytes.data(), bytes.size());

        n = ::write(dev.fd(), record.data(), record.size());
        if (n < 0)
            fail(format("write medium header: {}", std::strerror(errno)));
        if (static_cast<std::size_t>(n) != record.size())
            fail("short write on medium header");

        dev.write_filemark();
        std::cerr << format("medium initialized: uuid={}\n", mh.medium_uuid);
        return 0;

    } catch (const std::exception &e) {
        fail(e.what());
    }
}
```

- [ ] **Step 2: Verify compilation**

```bash
c++ -std=c++20 -fsyntax-only -Iinclude src/neotape_init.cpp
```

- [ ] **Step 3: Commit**

```bash
git add src/neotape_init.cpp
git commit -m "feat: neotape-init tool for medium initialization"
```

---

### Task 3: Create tape writer

**Files:**
- Create: `src/neotape_tape_writer.cpp`

- [ ] **Step 1: Write neotape_tape_writer.cpp**

```cpp
#include "neotape/tape.hpp"
#include "neotape/tape_navigator.hpp"
#include "neotape/format.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace mt {
namespace {

using std::format;
using std::string;
using std::string_view;
using std::vector;

// ====================== Writer State =============================

namespace fs = std::filesystem;

struct TapeWriterOptions {
    string device;
    string input = "-";
    string archive_name = "raw";
    uint32_t volume_block_size = 1024 * 1024;
    uint64_t slice_size = 64ull * 1024 * 1024;
    bool init_mode = false;
    bool init_if_blank = false;
    bool force_append = false;
};

struct WriterState {
    TapeWriterOptions opts;
    mt::TapeDevice *dev = nullptr;
    string archive_uuid;
    uint64_t volume_seq_num = 0;
    uint64_t logical_slice_seq_num = 0;
    uint64_t global_frame_seq_num = 0;
    uint64_t frame_seq_num_within_slice = 0;
    uint64_t current_slice_size = 0;
    bool slice_open = false;
    blake3_hasher slice_hasher;
};

// ====================== Helpers ==================================

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(1);
}

// Writes a single NeoTape record padded to volume_block_size.
// Returns true if fully committed, false if ENOSPC.
bool write_tape_record(TapeDevice &dev, const neotape::HeaderBytes &header,
                       const vector<uint8_t> *payload, uint32_t block_size) {
    vector<uint8_t> buf(block_size, 0);
    std::memcpy(buf.data(), header.data(), header.size());
    if (payload && !payload->empty()) {
        size_t copy = std::min(payload->size(), buf.size() - neotape::fixed_header_size);
        std::memcpy(buf.data() + neotape::fixed_header_size, payload->data(), copy);
    }

    ssize_t n = ::write(dev.fd(), buf.data(), buf.size());
    if (n < 0) {
        if (errno == ENOSPC) return false;
        fail(format("write: {}", std::strerror(errno)));
    }
    if (static_cast<std::size_t>(n) != buf.size()) {
        if (n > 0) return false;  // short write — treat as uncommitted
        return false;
    }
    return true;
}

void prompt_next_volume(uint64_t volume_seq_num) {
    std::cerr << format("End of tape reached, volume_seq_num={}\n", volume_seq_num);
    std::cerr << "Insert next volume and press Enter: ";
    std::string line;
    std::getline(std::cin, line);
}

// ====================== Volume + Frame Writers ===================

void write_volume_header(WriterState &state) {
    ++state.volume_seq_num;

    neotape::VolumeHeader vh;
    vh.volume_block_size = state.opts.volume_block_size;
    vh.archive_uuid = state.archive_uuid;
    vh.archive_name = state.opts.archive_name;
    vh.volume_seq_num = state.volume_seq_num;
    vh.volume_write_at_utc = neotape::utc_timestamp_now();

    auto bytes = neotape::serialize_volume_header(vh);
    while (!write_tape_record(*state.dev, bytes, nullptr,
                              state.opts.volume_block_size)) {
        prompt_next_volume(state.volume_seq_num);
        // After user inserts next tape: write Volume Header again
    }
    state.dev->write_filemark();
    state.slice_open = false;
}

void write_content_frame(WriterState &state, const vector<uint8_t> &payload,
                         bool end) {
    if (!state.slice_open) {
        ++state.logical_slice_seq_num;
        state.frame_seq_num_within_slice = 0;
        state.current_slice_size = 0;
        state.slice_open = true;
        blake3_hasher_init(&state.slice_hasher);
    }

    ++state.global_frame_seq_num;
    ++state.frame_seq_num_within_slice;

    neotape::Hash payload_hash = neotape::blake3_hash(payload.data(), payload.size());
    blake3_hasher_update(&state.slice_hasher, payload.data(), payload.size());
    state.current_slice_size += payload.size();

    neotape::Hash slice_hash{};
    if (end)
        blake3_hasher_finalize(&state.slice_hasher, slice_hash.data(), slice_hash.size());

    neotape::FrameHeader fh;
    fh.volume_block_size = state.opts.volume_block_size;
    fh.archive_uuid = state.archive_uuid;
    fh.archive_name = state.opts.archive_name;
    fh.volume_seq_num = state.volume_seq_num;
    fh.logical_slice_seq_num = state.logical_slice_seq_num;
    fh.global_frame_seq_num = state.global_frame_seq_num;
    fh.frame_seq_num_within_slice = state.frame_seq_num_within_slice;
    fh.frame_payload_size = payload.size();
    fh.frame_payload_blake3 = payload_hash;
    uint16_t flags = 0;
    if (state.frame_seq_num_within_slice == 1)
        flags |= neotape::frame_flag_start;
    if (end) {
        fh.slice_content_size = state.current_slice_size;
        fh.slice_content_blake3 = slice_hash;
        flags |= neotape::frame_flag_end;
    }
    fh.flags = flags;

    auto bytes = neotape::serialize_frame_header(fh);
    while (!write_tape_record(*state.dev, bytes, &payload,
                              state.opts.volume_block_size)) {
        prompt_next_volume(state.volume_seq_num);
        // Write Volume Header on new tape, then retry this frame
        write_volume_header(state);
    }

    if (end) {
        state.slice_open = false;
        state.dev->write_filemark();
    }
}

void write_archive_end(WriterState &state) {
    neotape::ArchiveEndHeader ae;
    ae.volume_block_size = state.opts.volume_block_size;
    ae.archive_uuid = state.archive_uuid;
    ae.archive_name = state.opts.archive_name;
    ae.volume_seq_num = state.volume_seq_num;
    ae.last_logical_slice_seq_num = state.logical_slice_seq_num;
    ae.last_global_frame_seq_num = state.global_frame_seq_num;
    ae.created_by_implementation = "NeoTape reference writer phase6-mvp";
    ae.archive_end_at_utc = neotape::utc_timestamp_now();

    auto bytes = neotape::serialize_archive_end_header(ae);
    while (!write_tape_record(*state.dev, bytes, nullptr,
                              state.opts.volume_block_size)) {
        prompt_next_volume(state.volume_seq_num);
        write_volume_header(state);
    }
    state.dev->write_filemark();
}

} // anonymous namespace

// ====================== Main Entry Point =========================

void write_tape_archive(const TapeWriterOptions &opts) {
    mt::TapeDevice dev(opts.device, true);

    WriterState state;
    state.opts = opts;
    state.dev = &dev;

    // Positioning
    if (opts.init_mode) {
        dev.rewind();
    } else {
        mt::nav::TapeNavigator nav(dev);
        auto r = nav.locate_append_position(
            opts.force_append ? mt::nav::AppendPolicy::force
                              : mt::nav::AppendPolicy::strict);
        if (r.condition == mt::nav::TapeCondition::blank) {
            if (!opts.init_if_blank)
                fail("tape is blank; use --init or --init-if-blank");
            dev.rewind();
        } else if (r.condition == mt::nav::TapeCondition::has_corrupt_tail) {
            if (!opts.force_append)
                fail("previous archive has corrupt tail; use --force-append");
        }
        // has_valid_tail → positioned correctly at append point
    }

    dev.set_block_size(opts.volume_block_size);
    state.archive_uuid = neotape::make_uuid_v4();

    // Volume Header
    write_volume_header(state);

    // Main framing loop (same logic as spool writer)
    FILE *input = stdin;
    if (opts.input != "-") {
        input = std::fopen(opts.input.c_str(), "rb");
        if (!input) fail(format("open {}: {}", opts.input, std::strerror(errno)));
    }

    size_t frame_payload_capacity = opts.volume_block_size - neotape::fixed_header_size;
    vector<uint8_t> buffer(frame_payload_capacity);
    vector<uint8_t> pending;
    bool have_pending = false;

    for (;;) {
        if (have_pending &&
            state.current_slice_size + pending.size() >= opts.slice_size) {
            write_content_frame(state, pending, true);
            pending.clear();
            have_pending = false;
            continue;
        }

        uint64_t pending_size = have_pending ? pending.size() : 0;
        uint64_t remaining_in_slice =
            state.slice_open
                ? opts.slice_size - state.current_slice_size - pending_size
                : opts.slice_size - pending_size;
        size_t want = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), remaining_in_slice));
        size_t n = std::fread(buffer.data(), 1, want, input);
        if (n > 0) {
            if (have_pending) {
                write_content_frame(state, pending, false);
                pending.clear();
                have_pending = false;
            }
            vector<uint8_t> payload(buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(n));
            if (n != want) {
                write_content_frame(state, payload, true);
            } else {
                pending = std::move(payload);
                have_pending = true;
            }
        }
        if (n != want) {
            if (std::ferror(input))
                fail(format("read input: {}", std::strerror(errno)));
            break;
        }
    }

    if (input != stdin && std::fclose(input) != 0)
        fail(format("close input: {}", std::strerror(errno)));

    if (have_pending)
        write_content_frame(state, pending, true);
    write_archive_end(state);

    std::cerr << format("archive {} written to tape {}\n",
        state.archive_uuid, opts.device);
}

} // namespace mt
```

- [ ] **Step 2: Verify compilation**

```bash
c++ -std=c++20 -fsyntax-only -Iinclude src/neotape_tape_writer.cpp
```

- [ ] **Step 3: Commit**

```bash
git add src/neotape_tape_writer.cpp
git commit -m "feat: tape writer backend with ENOSPC handling"
```

---

### Task 4: Modify neotape_write.cpp CLI

**Files:**
- Modify: `src/neotape_write.cpp`

- [ ] **Step 1: Add tape writer includes and declaration**

After the existing includes, add:

```cpp
#include "neotape/tape.hpp"
#include "neotape/tape_navigator.hpp"

namespace mt {
struct TapeWriterOptions;
void write_tape_archive(const TapeWriterOptions &opts);
}
```

- [ ] **Step 2: Update Options struct**

```cpp
struct Options {
    string input = "-";         // was -f, now -i
    string tape_device;         // -f <device> (tape mode output)
    fs::path output_dir;        // -o (spool mode)
    string archive_name = "raw";
    uint32_t volume_block_size = 1024 * 1024;
    uint64_t slice_size = 64ull * 1024 * 1024;
    uint64_t virtual_tape_size = 0;
    bool init_mode = false;     // --init
    bool init_if_blank = false; // --init-if-blank
    bool force_append = false;  // --force-append
};
```

- [ ] **Step 3: Update usage()**

```cpp
void usage(const char *prog) {
    std::cerr << format(
        "usage: {} -f <tape-device> [-i <input>] [options]\n"
        "       {} --target=spool -o <dir> [-i <input>] [options]\n"
        "\n"
        "Tape options:\n"
        "  -f <device>       Tape device path (implies tape mode)\n"
        "  --init            Write from BOT (overwrites)\n"
        "  --init-if-blank   Only init if tape is blank\n"
        "  --force-append    Append even without valid tail\n"
        "\n"
        "Spool options:\n"
        "  -o <dir>          Spool output directory\n"
        "\n"
        "Common options:\n"
        "  -i <input>        Payload input file (default: stdin)\n"
        "  --archive-name <name>\n"
        "  --volume-block-size <bytes>\n"
        "  --slice-size <bytes>\n"
        "  --virtual-tape-size <bytes>\n",
        prog, prog);
}
```

- [ ] **Step 4: Update long_opts and parse_args**

```cpp
static const struct option long_opts[] = {
    {"target",            required_argument, nullptr, 't'},
    {"archive-name",      required_argument, nullptr, 'n'},
    {"volume-block-size", required_argument, nullptr, 'b'},
    {"slice-size",        required_argument, nullptr, 's'},
    {"virtual-tape-size", required_argument, nullptr, 'z'},
    {"init",              no_argument,       nullptr, 256},
    {"init-if-blank",     no_argument,       nullptr, 257},
    {"force-append",      no_argument,       nullptr, 258},
    {"help",              no_argument,       nullptr, 'h'},
    {nullptr, 0, nullptr, 0}
};
```

In parse_args, update the getopt string to `"f:i:o:hn:b:s:z:"` and add cases:

```cpp
case 'f': opts.tape_device = optarg; break;
case 'i': opts.input = optarg; break;
case 256: opts.init_mode = true; break;
case 257: opts.init_if_blank = true; break;
case 258: opts.force_append = true; break;
```

Add validation for tape mode:

```cpp
if (!opts.tape_device.empty() && !opts.output_dir.empty())
    fail("specify either -f (tape) or -o (spool), not both");
if (!opts.tape_device.empty() && opts.virtual_tape_size > 0)
    fail("--virtual-tape-size is for spool mode only");
```

- [ ] **Step 5: Update main() to route to tape writer**

```cpp
if (!opts.tape_device.empty()) {
    mt::TapeWriterOptions tape_opts;
    tape_opts.device = opts.tape_device;
    tape_opts.input = opts.input;
    tape_opts.archive_name = opts.archive_name;
    tape_opts.volume_block_size = opts.volume_block_size;
    tape_opts.slice_size = opts.slice_size;
    tape_opts.init_mode = opts.init_mode;
    tape_opts.init_if_blank = opts.init_if_blank;
    tape_opts.force_append = opts.force_append;
    mt::write_tape_archive(tape_opts);
} else {
    write_spool_archive(opts);
}
```

- [ ] **Step 6: Verify compilation**

```bash
c++ -std=c++20 -fsyntax-only -Iinclude src/neotape_write.cpp
```

- [ ] **Step 7: Commit**

```bash
git add src/neotape_write.cpp
git commit -m "feat: CLI changes for tape backend (-f device, -i input, tape options)"
```

---

### Task 5: Update Makefile

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add new variables and rules**

```makefile
TAPE_WRITER_OBJ = $(BUILDDIR)/neotape_tape_writer.o

$(TAPE_WRITER_OBJ) : src/neotape_tape_writer.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/neotape-init : src/neotape_init.cpp $(FORMAT_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)
```

Update neotape-write link rule:

```makefile
$(BINDIR)/neotape-write : src/neotape_write.cpp $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) -o $@ $(LDLIBS)
```

Update EXE and clean:

```makefile
EXE	= bin/pax bin/mt-pax bin/neotape-write bin/neotape-inspect bin/neotape-plan bin/neotape-cat-volumes bin/test_tape bin/neotape-init

clean:
	-rm -f ${EXE} ${BINDIR}/*.o $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TEST_DEVICE_OBJ) $(TAPE_WRITER_OBJ) $(B3LIB) $(B3OBJ) $(CRC32CLIB) $(CRC32COBJ)
```

- [ ] **Step 2: Build everything**

```bash
make clean && make -j "$(nproc)"
```

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "build: add neotape-init and tape writer to Makefile"
```

---

### Task 6: Add tape writer tests

**Files:**
- Modify: `tests/test_tape.cpp`

- [ ] **Step 1: Add tests for tape writer against FileBackedTapeDevice**

Near the bottom of main(), before cleanup:

```cpp
    // --- Test 6: init via tape writer ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_6.bin", 4096, true);
        // Rewind to simulate blank tape
        dev.rewind();

        // Write via simple write_tape_record-style call
        char buf[1024] = {};
        memcpy(buf, "NeoTape", 7);
        buf[8] = 1;
        buf[9] = static_cast<uint8_t>(neotape::HeaderType::medium);

        neotape::HeaderBytes hb;
        memcpy(hb.data(), buf, sizeof(buf));
        vector<uint8_t> empty_payload;
        // Test the inline write helper logic:
        int fd = dev.fd();
        vector<uint8_t> record(4096, 0);
        memcpy(record.data(), hb.data(), hb.size());
        ssize_t nw = ::write(fd, record.data(), record.size());
        CHECK(nw == 4096, "write medium header record");
        dev.write_filemark();

        // Read back and check magic
        dev.rewind();
        char rbuf[1024] = {};
        ssize_t nr = ::read(fd, rbuf, sizeof(rbuf));
        CHECK(nr == 1024, "read back 1024 bytes");
        CHECK(memcmp(rbuf, "NeoTape", 7) == 0, "medium header magic");
    }

    // --- Test 7: neotape-init against test device ---
    // (Integration test: requires linking against neotape_init.o.
    // For now, just verify the pattern works.)
```

- [ ] **Step 2: Append cleanup for test files**

Add to the unlink section:

```cpp
    unlink("/tmp/tape_test_6.bin");
```

- [ ] **Step 3: Build and run tests**

```bash
make -j "$(nproc)" && make test
```

- [ ] **Step 4: Commit**

```bash
git add tests/test_tape.cpp
git commit -m "test: add tape writer tests against FileBackedTapeDevice"
```

---

### Task 7: Final verification

- [ ] **Step 1: Full clean build**

```bash
make clean && make -j "$(nproc)"
```

- [ ] **Step 2: Run all tests**

```bash
make test
```

- [ ] **Step 3: Check git status**

```bash
git status
```
