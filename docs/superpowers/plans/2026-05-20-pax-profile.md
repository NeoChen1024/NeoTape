# Pax Payload Profile Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement `--payload-profile=pax` that reads pax entries from stdin via libarchive and aligns slice boundaries at pax entry boundaries (no file split across slices).

**Architecture:** New `src/neotape_pax_writer.cpp` with a `write_pax_archive()` function that takes a backend interface (callbacks for volume/slice/archive-end operations). Called from `neotape_write.cpp` when pax profile is selected. Uses `archive_read` to parse stdin, one frame buffer (`volume_block_size - 1024`) as memory footprint — same as raw profile.

**Tech Stack:** C++20, libarchive (already linked), BLAKE3, CRC32C.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `include/neotape/pax_writer.hpp` | Create | Backend callback interface, `write_pax_archive()` declaration |
| `src/neotape_pax_writer.cpp` | Create | Pax entry reader + framing loop |
| `src/neotape_tape_writer.cpp` | Modify | Wire tape backend functions into pax archive calls |
| `src/neotape_write.cpp` | Modify | Wire spool backend functions + routing |
| `Makefile` | Modify | Add `PAX_WRITER_OBJ` |
| `include/neotape/format.hpp` | (already done) | `PayloadProfile::pax` enum value added |

---

### Task 1: Create `include/neotape/pax_writer.hpp`

**Files:**
- Create: `include/neotape/pax_writer.hpp`

The interface for the pax writer. Both tape and spool backends implement the callbacks via a struct of function pointers:

```cpp
#pragma once

#include "neotape/format.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace neotape {

// Backend interface for receiving framed NeoTape output.
// Tape and spool backends each provide their own implementation.
struct PaxBackend {
    std::function<void(uint64_t volume_seq_num)> write_volume_header;
    std::function<void(const std::vector<uint8_t>& payload, bool end_slice)> write_content_frame;
    std::function<void()> write_archive_end;
    std::function<void(const std::string&)> fail;
};

// Read pax entries from fd (typically STDIN_FILENO), slice at entry
// boundaries, and emit NeoTape frames through the backend.
// Memory: one frame payload buffer (volume_block_size - 1024).
void write_pax_archive(int input_fd, uint32_t volume_block_size,
                       uint64_t slice_size, const PaxBackend &backend);

} // namespace neotape
```

---

### Task 2: Create `src/neotape_pax_writer.cpp`

**Files:**
- Create: `src/neotape_pax_writer.cpp`

```cpp
#include "neotape/pax_writer.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace neotape {

void write_pax_archive(int input_fd, uint32_t volume_block_size,
                       uint64_t slice_size, const PaxBackend &backend) {
    struct archive *a = archive_read_new();
    archive_read_support_format_pax(a);

    if (archive_read_open_fd(a, input_fd, volume_block_size) != ARCHIVE_OK) {
        backend.fail(std::string("archive_read_open: ") +
                     archive_error_string(a));
    }

    size_t frame_payload_capacity = volume_block_size - fixed_header_size;
    std::vector<uint8_t> buf(frame_payload_capacity);
    std::vector<uint8_t> pending;
    bool have_pending = false;
    uint64_t current_slice_size = 0;
    uint64_t volume_seq_num = 0;
    uint64_t logical_slice_seq_num = 0;
    bool slice_open = false;
    bool first_entry = true;

    struct archive_entry *entry;
    int rc;
    while ((rc = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        // Check if we should end the previous slice before this entry.
        // We keep the slice open if it hasn't hit slice_size yet.
        // If it has, the slice was already closed by the previous entry's
        // last frame (which had end_slice=true).

        for (;;) {
            la_ssize_t n = archive_read_data(a, buf.data(), buf.size());
            if (n < 0) {
                backend.fail(std::string("archive_read_data: ") +
                             archive_error_string(a));
            }
            if (n == 0) break;  // end of this entry

            if (n > 0) {
                bool entry_data_done =
                    (n < static_cast<la_ssize_t>(buf.size()));
                bool hit_slice_target =
                    current_slice_size + (have_pending ? pending.size() : 0) +
                        static_cast<uint64_t>(n) >= slice_size;
                bool end_slice = entry_data_done && hit_slice_target;

                // Same buffer-one-ahead logic as raw profile
                if (have_pending) {
                    backend.write_content_frame(pending, end_slice && entry_data_done);
                    if (end_slice) {
                        current_slice_size = 0;
                        slice_open = false;
                    }
                    pending.clear();
                    have_pending = false;
                    // If we already ended the slice, don't also end on this payload
                    if (end_slice) {
                        end_slice = false;
                    }
                }

                std::vector<uint8_t> payload(
                    buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
                if (!entry_data_done) {
                    // More data follows for this entry
                    if (have_pending) {
                        backend.write_content_frame(pending, false);
                        pending.clear();
                    }
                    pending = std::move(payload);
                    have_pending = true;
                } else {
                    // Last chunk of this entry
                    backend.write_content_frame(payload, end_slice);
                    current_slice_size += payload.size();
                    if (end_slice) {
                        current_slice_size = 0;
                    } else {
                        current_slice_size += payload.size();
                    }
                }
            }
        }
    }

    if (rc != ARCHIVE_EOF) {
        backend.fail(std::string("archive_read: ") +
                     archive_error_string(a));
    }

    archive_read_close(a);
    archive_read_free(a);

    if (have_pending)
        backend.write_content_frame(pending, true);
    backend.write_archive_end();
}

} // namespace neotape
```

Note: The entry loop naturally aligns slices at entry boundaries. When `end_slice=true`, the frame is emitted with the END flag (via the callback), and the next iteration starts a new slice. The `slice_open` tracking is managed inside the callback implementations (tape/spool), not here.

---

### Task 3: Wire tape backend into pax writer

**Files:**
- Modify: `src/neotape_tape_writer.cpp`

Add a new function `write_pax_tape_archive()` that constructs the `PaxBackend` callbacks using the existing tape helpers (`write_volume_header`, `write_content_frame`, `write_archive_end`).

```cpp
void write_pax_tape_archive(const TapeWriterOptions &opts) {
    TapeDevice dev(opts.device, true);
    dev.set_block_size(opts.volume_block_size);

    // State that the callbacks close over
    struct State {
        TapeDevice *dev;
        string archive_uuid;
        uint64_t volume_seq_num = 0;
        uint64_t logical_slice_seq_num = 0;
        bool slice_open = false;
        blake3_hasher slice_hasher;
        TapeWriterOptions opts;
    };
    auto st = std::make_shared<State>();
    st->dev = &dev;
    st->opts = opts;
    st->archive_uuid = neotape::make_uuid_v4();

    // Positioning (same as raw)
    if (opts.init_mode) {
        dev.rewind();
    } else {
        nav::TapeNavigator nav(dev);
        auto r = nav.locate_append_position(
            opts.force_append ? nav::AppendPolicy::force : nav::AppendPolicy::strict);
        if (r.condition == nav::TapeCondition::blank && !opts.init_if_blank)
            fail("tape is blank");
        if (r.condition == nav::TapeCondition::has_corrupt_tail && !opts.force_append)
            fail("corrupt tail");
    }

    neotape::PaxBackend backend;
    backend.write_volume_header = [st](uint64_t vsn) {
        st->volume_seq_num = vsn;
        write_volume_header_internal(st);  // internal helper
    };
    backend.write_content_frame = [st](const vector<uint8_t> &payload, bool end_slice) {
        write_content_frame_internal(st, payload, end_slice);
    };
    backend.write_archive_end = [st]() {
        write_archive_end_internal(st);
    };
    backend.fail = [](const string &msg) { fail(msg); };

    neotape::write_pax_archive(STDIN_FILENO, opts.volume_block_size,
                               opts.slice_size, backend);

    std::cerr << format("archive {} written to tape {}\n",
        st->archive_uuid, opts.device);
}
```

The internal helper functions (`write_volume_header_internal`, etc.) are extracted from the existing anonymous-namespace helpers, modified to take the shared state and not depend on `WriterState` encapsulation.

Alternatively, if this is too complex, the simpler approach: put the pax entry loop directly in `write_tape_archive` as a branch, reusing the existing `WriterState` and helpers directly. This avoids extracting the helpers.

Recommended: **inline in `write_tape_archive` as a branch**:

```cpp
if (opts.payload_profile == "pax") {
    // pax entry loop using libarchive
    // same WriterState, same write_volume_header/write_content_frame/write_archive_end
    // different framing: aligned at entry boundaries
} else {
    // existing raw loop
}
```

This is the simplest implementation. The pax branch shares all the same helpers.

---

### Task 4: Wire spool backend into pax writer

**Files:**
- Modify: `src/neotape_write.cpp`

Add a pax branch in `write_spool_archive()` alongside the existing raw loop, similar to the tape approach. The pax branch uses `write_pax_archive` with callbacks that call the existing spool helpers (`write_volume_header`, `write_content_frame`, `write_archive_end` from the spool file's anonymous namespace).

Or simpler: add the pax entry loop inline as an `if` branch in the existing spool archive function.

---

### Task 5: CLI routing

**Files:**
- Modify: `src/neotape_write.cpp`

In `main()`, when `opts.payload_profile == "pax"`, route to the pax-aware tape/spool functions. The `pax_sources` vector is removed from Options (not needed — stdin is the pax source).

---

### Task 6: Build + Makefile

**Files:**
- Modify: `Makefile`

```makefile
PAX_WRITER_OBJ = $(BUILDDIR)/neotape_pax_writer.o

$(PAX_WRITER_OBJ) : src/neotape_pax_writer.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/neotape-write : ... $(PAX_WRITER_OBJ) ...
```

---

### Verification

```sh
make clean && make -j "$(nproc)" && make test

# Functional test:
echo "hello" > /tmp/testfile
(cd /tmp && bin/pax -f - testfile) | bin/neotape-write --target=spool -o /tmp/pax_test --payload-profile=pax

# Verify:
bin/neotape-cat-volumes /tmp/pax_test | bsdtar -xpf - -C /tmp/pax_restore
diff /tmp/testfile /tmp/pax_restore/testfile
```
