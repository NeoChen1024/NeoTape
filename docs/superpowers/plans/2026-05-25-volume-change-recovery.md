# Volume Change Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement safe read/write volume-change recovery for real tape, including prompt control, wrong-volume rejection, and frame/slice mismatch failure.

**Architecture:** Reuse the existing `neotape::prompt_for_volume_change()` and `--control=auto|none` model. Add control-policy plumbing to tape writer options, factor restore identity validation into small helper functions, and keep frame/slice sequence mismatches as hard failures rather than recoverable prompts.

**Tech Stack:** C++20, GNU Make, existing NeoTape format helpers, existing `TapeDevice` / `SpoolTapeDevice` abstractions, existing no-framework C++ test binaries.

---

## Reference Spec

Read first: `docs/superpowers/specs/2026-05-25-volume-change-recovery-design.md`

## File Structure

- Modify: `include/neotape/tape_writer.hpp`
  - Add `neotape::ControlPolicy control` to `mt::TapeWriterOptions`.
  - Include `neotape/cli.hpp` or forward-declare safely.
- Modify: `src/neotape_write.cpp`
  - Copy parsed raw-write and backup `--control` into `TapeWriterOptions`.
- Modify: `src/neotape_tape_writer.cpp`
  - Replace simple Enter-only `prompt_next_volume()` with shared prompt handling.
  - Handle `--control=none`, abort, continue-current, and replacement tape locators.
  - Preserve archive UUID across volume changes and increment volume sequence correctly.
- Modify: `src/neotape_cat_volumes.cpp`
  - Add restore identity state.
  - Validate Volume Header identity before processing frames on subsequent volumes.
  - Prompt for next volume only at clean volume boundary.
  - Fail immediately for frame/slice mismatch.
- Create: `include/neotape/restore_validation.hpp`
  - Define restore identity/read-state structs and validation functions usable by the reader and tests.
- Create: `src/neotape_restore_validation.cpp`
  - Implement Volume Header, Frame Header, and Archive End validation without I/O side effects.
- Modify: `tests/test_cli.cpp`
  - Add narrow tests for prompt result / policy types if needed.
- Modify: `tests/test_tape.cpp`
  - Add tests for writer control-policy plumbing where feasible with spool-backed device.
- Create: `tests/test_restore_validation.cpp`
  - Unit-test restore identity and frame/slice validation helpers without real tape.
- Modify: `Makefile`
  - Build `src/neotape_restore_validation.o` into `bin/neotape`, `bin/neotape-cat-volumes`, and `bin/test_restore_validation`.
  - Run `bin/test_restore_validation` in `make test`.
- Modify: `docs/superpowers/plans/2026-05-25-real-tape-neotape-validation.md`
  - Update expected manual behavior now that prompt code exists.

Do not commit during execution unless the user explicitly requests a commit.

### Task 1: Add Restore Validation Test Binary Skeleton

**Files:**
- Create: `tests/test_restore_validation.cpp`
- Modify: `Makefile:8,90-101`

- [ ] **Step 1: Create the failing test binary skeleton**

Create `tests/test_restore_validation.cpp`:

```cpp
#include "neotape/format.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool cond, const std::string &msg) {
    if (!cond) {
        std::cerr << "test_restore_validation: " << msg << "\n";
        std::exit(1);
    }
}

void test_binary_runs() {
    neotape::VolumeHeader vh;
    vh.volume_block_size = 4096;
    vh.archive_uuid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    vh.archive_name = "restore-test";
    vh.volume_seq_num = 1;
    vh.payload_profile = neotape::PayloadProfile::pax;
    vh.volume_write_at_utc = "2026-05-25T00:00:00Z";

    auto bytes = neotape::serialize_volume_header(vh);
    auto parsed = neotape::parse_fixed_header(bytes.data(), bytes.size());
    require(parsed.volume.has_value(), "volume header parser smoke");
}

} // namespace

int main() {
    test_binary_runs();
    return 0;
}
```

- [ ] **Step 2: Wire the binary into the Makefile**

Update `Makefile` so `EXE` includes `bin/test_restore_validation`, add a build rule, and run it under `test`:

```make
EXE	= bin/mt-pax bin/neotape bin/neotape-write bin/neotape-inspect bin/neotape-plan bin/neotape-cat-volumes bin/test_tape bin/test_cli bin/test_restore_validation bin/neotape-init
```

Add after the `bin/test_cli` rule:

```make
$(BINDIR)/test_restore_validation : tests/test_restore_validation.cpp $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)
```

Update `test` target:

```make
test: $(BINDIR)/test_tape $(BINDIR)/test_cli $(BINDIR)/test_restore_validation $(BINDIR)/neotape
	$(BINDIR)/test_tape
	$(BINDIR)/test_cli
	$(BINDIR)/test_restore_validation
	sh tests/smoke_pax_backup_restore.sh
	sh tests/smoke_tape_backup_wiring.sh
```

- [ ] **Step 3: Run the new test binary**

Run:

```bash
make bin/test_restore_validation && bin/test_restore_validation
```

Expected: build succeeds and command exits `0`.

- [ ] **Step 4: Run existing tests**

Run:

```bash
make test
```

Expected: existing tests still pass. If unrelated existing failures appear, record them before continuing.

### Task 2: Add Tape Writer Control Policy Plumbing

**Files:**
- Modify: `include/neotape/tape_writer.hpp`
- Modify: `src/neotape_write.cpp:831-886`
- Test: `tests/test_tape.cpp`

- [ ] **Step 1: Write a compile-time test for default writer control policy**

In `tests/test_tape.cpp`, add this check near the existing callback writer test setup:

```cpp
        CHECK(opts.control == neotape::ControlPolicy::auto_prompt,
              "tape writer defaults to auto control policy");
```

Expected: this fails to compile because `TapeWriterOptions::control` does not exist.

- [ ] **Step 2: Run the failing test build**

Run:

```bash
make bin/test_tape
```

Expected: compile failure mentioning `control` is not a member of `mt::TapeWriterOptions`.

- [ ] **Step 3: Add control policy to writer options**

Modify `include/neotape/tape_writer.hpp`:

```cpp
#include "neotape/cli.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
```

Add to `struct TapeWriterOptions`:

```cpp
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
```

- [ ] **Step 4: Propagate raw write control**

In `src/neotape_write.cpp`, inside `neotape_write_main()`, after setting `opts.volume_block_size`, add:

```cpp
        opts.control = raw.control;
```

Then in `run_writer()`, after `tape_opts.payload_profile = opts.payload_profile;`, add:

```cpp
        tape_opts.control = opts.control;
```

- [ ] **Step 5: Propagate backup control**

In `run_spool_pax_backup()`, after `tape_opts.payload_profile = "pax";`, add:

```cpp
    tape_opts.control = backup.control;
```

In `run_tape_pax_backup()`, after `opts.payload_profile = "pax";`, add:

```cpp
    opts.control = backup.control;
```

- [ ] **Step 6: Run test build and tests**

Run:

```bash
make bin/test_tape && bin/test_tape
```

Expected: test binary builds and exits `0`.

### Task 3: Replace Writer Enter-Only Prompt With Shared Prompt

**Files:**
- Modify: `src/neotape_tape_writer.cpp:1-186`
- Test: `tests/test_tape.cpp`

- [ ] **Step 1: Add a test hook for prompt-disabled writer behavior**

In `tests/test_tape.cpp`, add a `FailingWriteTapeDevice` class in the anonymous namespace:

```cpp
class FailingWriteTapeDevice final : public mt::TapeDevice {
public:
    FailingWriteTapeDevice() : TapeDevice(-1, "/dev/test-failing-write", true) {}

    int fd() const noexcept override { return pipe_write_fd_; }

    void set_fd(int fd) noexcept { pipe_write_fd_ = fd; }


protected:
    void do_mtop(int op, int count) override {
        ops_.push_back({op, count});
        if (op == mt::MTSETBLK || op == mt::MTREW || op == mt::MTWEOF)
            return;
        throw mt::Error(device_path(), "mtop", ENOTSUP);
    }

public:
    std::vector<std::pair<int, int>> ops_;

private:
    int pipe_write_fd_ = -1;
};
```

Add this test before final failure report:

```cpp
    // --- Test 10: writer honors --control=none on impossible write ---
    {
        int fds[2];
        CHECK(::pipe(fds) == 0, "pipe for failing writer test");
        ::close(fds[0]);

        FailingWriteTapeDevice dev;
        dev.set_fd(fds[1]);

        mt::TapeWriterOptions opts;
        opts.device = "/dev/test-failing-write";
        opts.archive_name = "no-prompt";
        opts.volume_block_size = 4096;
        opts.payload_profile = "pax";
        opts.init_mode = true;
        opts.control = neotape::ControlPolicy::none;

        bool threw = false;
        try {
            mt::write_tape_archive_from_chunks_to_device(
                dev, opts, [](mt::TapeChunkWriter) {});
        } catch (const std::exception &e) {
            threw = std::string(e.what()).find("volume change required") != std::string::npos;
        }
        CHECK(threw, "writer control=none fails instead of prompting");
        ::close(fds[1]);
    }
```

Note: this uses a closed pipe to force write failure. If Linux reports `EPIPE` instead of `ENOSPC`, adjust the test double during implementation by adding a protected test-only writer seam instead of relying on pipe behavior.

- [ ] **Step 2: Run the failing test**

Run:

```bash
make bin/test_tape && bin/test_tape
```

Expected: the new test fails because writer currently uses the old prompt path or fails with a different message.

- [ ] **Step 3: Include CLI prompt support in tape writer**

At the top of `src/neotape_tape_writer.cpp`, add:

```cpp
#include "neotape/cli.hpp"
```

- [ ] **Step 4: Replace `prompt_next_volume()` helper**

Delete the existing `prompt_next_volume(uint64_t)` and replace it with:

```cpp
void handle_volume_change(WriterState &state, uint64_t expected_volume_seq_num) {
    neotape::require_prompt_allowed(state.opts.control);

    neotape::VolumePromptRequest req;
    req.archive_uuid = state.archive_uuid;
    req.expected_volume = expected_volume_seq_num;
    req.current_locator = neotape::Locator{"tape", state.opts.device};
    req.write_mode = true;

    auto result = neotape::prompt_for_volume_change(req);
    if (result.choice == neotape::VolumePromptChoice::abort)
        throw std::runtime_error("volume change aborted by user");
    if (result.choice == neotape::VolumePromptChoice::change_locator) {
        if (!result.replacement_locator || result.replacement_locator->kind != "tape")
            throw std::runtime_error("replacement locator must be tape:<device>");
        state.opts.device = result.replacement_locator->locator;
        throw std::runtime_error("change device requested but writer device replacement is not wired yet");
    }
    if (result.choice != neotape::VolumePromptChoice::continue_current)
        throw std::runtime_error("unsupported volume prompt choice");
}
```

This step only establishes shared prompt behavior for `--control=none`, abort, and retry-current. Task 4 wires replacement-device support.

- [ ] **Step 5: Call the shared prompt on write failure**

In `write_volume_header()` replace:

```cpp
        prompt_next_volume(state.volume_seq_num);
```

with:

```cpp
        handle_volume_change(state, state.volume_seq_num);
```

In `write_content_frame()` replace:

```cpp
        prompt_next_volume(state.volume_seq_num);
```

with:

```cpp
        handle_volume_change(state, state.volume_seq_num + 1);
```

In `write_archive_end()` replace:

```cpp
        prompt_next_volume(state.volume_seq_num);
```

with:

```cpp
        handle_volume_change(state, state.volume_seq_num + 1);
```

- [ ] **Step 6: Run tests**

Run:

```bash
make bin/test_tape && bin/test_tape
```

Expected: test binary exits `0` or exposes the need for the device-replacement refactor in Task 4.

### Task 4: Wire Writer Replacement Device Support

**Files:**
- Modify: `src/neotape_tape_writer.cpp:29-317`
- Test: `tests/test_tape.cpp`

- [ ] **Step 1: Refactor writer state to own replacement devices**

Modify `WriterState` in `src/neotape_tape_writer.cpp`:

```cpp
    TapeDevice *dev = nullptr;
    std::unique_ptr<TapeDevice> owned_dev;
```

Add include:

```cpp
#include <memory>
```

- [ ] **Step 2: Add helper to configure current writer device**

Above `initialize_for_write()`, add:

```cpp
void configure_writer_device(WriterState &state) {
    state.dev->configure_preferred_variable_block_mode(
        state.opts.volume_block_size, "neotape-write archive records", std::cerr);
}
```

Replace the direct `dev.configure_preferred_variable_block_mode(...)` call in `initialize_for_write()` with:

```cpp
    configure_writer_device(state);
```

Make sure `state.dev = &dev;` is assigned before this call.

- [ ] **Step 3: Replace the incomplete change-device branch**

Replace the `change_locator` branch in `handle_volume_change()` with:

```cpp
    if (result.choice == neotape::VolumePromptChoice::change_locator) {
        if (!result.replacement_locator || result.replacement_locator->kind != "tape")
            throw std::runtime_error("replacement locator must be tape:<device>");
        state.opts.device = result.replacement_locator->locator;
        state.owned_dev = std::make_unique<TapeDevice>(state.opts.device, true);
        state.dev = state.owned_dev.get();
        configure_writer_device(state);
        state.dev->rewind();
        return;
    }
```

- [ ] **Step 4: Ensure volume headers are written after replacement**

Keep existing calls to `write_volume_header(state)` after failed content/archive-end writes. They increment `volume_seq_num` and write the next header. For failures while writing the Volume Header itself, `handle_volume_change(state, state.volume_seq_num)` must not increment sequence again; the loop retries the same header number.

- [ ] **Step 5: Run writer tests**

Run:

```bash
make bin/test_tape && bin/test_tape
```

Expected: test binary exits `0`.

### Task 5: Add Restore Validation Helper API And Tests

**Files:**
- Create: `include/neotape/restore_validation.hpp`
- Create: `src/neotape_restore_validation.cpp`
- Modify: `tests/test_restore_validation.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Create the restore validation header**

Create `include/neotape/restore_validation.hpp`:

```cpp
#pragma once

#include "neotape/format.hpp"

#include <cstdint>
#include <string>

namespace neotape {

struct RestoreValidationState {
    std::string archive_uuid;
    std::string archive_name;
    PayloadProfile payload_profile = PayloadProfile::raw;
    uint64_t expected_volume_seq_num = 1;
    uint64_t current_volume_seq_num = 0;
    uint64_t expected_global_frame_seq_num = 1;
    uint64_t expected_logical_slice_seq_num = 1;
    uint64_t current_slice_size = 0;
    uint32_t volume_block_size = 0;
    bool identity_established = false;
    bool slice_open = false;
};

void accept_restore_volume_header(const VolumeHeader &vh,
                                  RestoreValidationState &state);
void validate_restore_frame_header(const FrameHeader &fh,
                                   const RestoreValidationState &state);
void note_restore_frame_accepted(const FrameHeader &fh,
                                 RestoreValidationState &state);
void validate_restore_archive_end(const ArchiveEndHeader &ae,
                                  const RestoreValidationState &state);

} // namespace neotape
```

- [ ] **Step 2: Create the restore validation implementation**

Create `src/neotape_restore_validation.cpp`:

```cpp
#include "neotape/restore_validation.hpp"

#include <format>
#include <stdexcept>

namespace neotape {

using std::format;

void accept_restore_volume_header(const VolumeHeader &vh,
                                  RestoreValidationState &state) {
    if (!state.identity_established) {
        state.archive_uuid = vh.archive_uuid;
        state.archive_name = vh.archive_name;
        state.payload_profile = vh.payload_profile;
        state.volume_block_size = vh.volume_block_size;
        state.expected_volume_seq_num = vh.volume_seq_num;
        state.identity_established = true;
    }

    if (vh.archive_uuid != state.archive_uuid)
        throw std::runtime_error(format(
            "volume archive uuid mismatch: expected {}, got {}",
            state.archive_uuid, vh.archive_uuid));
    if (vh.volume_seq_num != state.expected_volume_seq_num)
        throw std::runtime_error(format(
            "volume sequence mismatch: expected {}, got {}",
            state.expected_volume_seq_num, vh.volume_seq_num));
    if (vh.volume_block_size != state.volume_block_size)
        throw std::runtime_error(format(
            "volume block size mismatch: expected {}, got {}",
            state.volume_block_size, vh.volume_block_size));
    if (vh.payload_profile != state.payload_profile)
        throw std::runtime_error("volume payload profile mismatch");

    state.current_volume_seq_num = vh.volume_seq_num;
    ++state.expected_volume_seq_num;
}

void validate_restore_frame_header(const FrameHeader &fh,
                                   const RestoreValidationState &state) {
    if (!state.identity_established)
        throw std::runtime_error("frame before volume header");
    if (fh.archive_uuid != state.archive_uuid)
        throw std::runtime_error(format(
            "frame archive uuid mismatch: expected {}, got {}",
            state.archive_uuid, fh.archive_uuid));
    if (fh.volume_seq_num != state.current_volume_seq_num)
        throw std::runtime_error(format(
            "frame volume sequence mismatch: expected {}, got {}",
            state.current_volume_seq_num, fh.volume_seq_num));
    if (fh.volume_block_size != state.volume_block_size)
        throw std::runtime_error(format(
            "frame block size mismatch: expected {}, got {}",
            state.volume_block_size, fh.volume_block_size));
    if (fh.global_frame_seq_num != state.expected_global_frame_seq_num)
        throw std::runtime_error(format(
            "frame sequence mismatch on volume {}: expected {}, got {}",
            state.current_volume_seq_num, state.expected_global_frame_seq_num,
            fh.global_frame_seq_num));

    bool start = (fh.flags & frame_flag_start) != 0;
    if (start && fh.logical_slice_seq_num != state.expected_logical_slice_seq_num)
        throw std::runtime_error(format(
            "slice sequence mismatch on volume {}: expected {}, got {}",
            state.current_volume_seq_num, state.expected_logical_slice_seq_num,
            fh.logical_slice_seq_num));
}

void note_restore_frame_accepted(const FrameHeader &fh,
                                 RestoreValidationState &state) {
    bool start = (fh.flags & frame_flag_start) != 0;
    bool end = (fh.flags & frame_flag_end) != 0;
    if (start) {
        state.slice_open = true;
        state.current_slice_size = 0;
    }
    state.current_slice_size += fh.frame_payload_size;
    ++state.expected_global_frame_seq_num;
    if (end) {
        state.slice_open = false;
        ++state.expected_logical_slice_seq_num;
    }
}

void validate_restore_archive_end(const ArchiveEndHeader &ae,
                                  const RestoreValidationState &state) {
    if (state.slice_open)
        throw std::runtime_error("archive ended with open slice");
    if (!(ae.flags & archive_end_flag_clean_end))
        throw std::runtime_error("archive end missing CLEAN_END flag");
    if (ae.archive_uuid != state.archive_uuid)
        throw std::runtime_error(format(
            "archive end uuid mismatch: expected {}, got {}",
            state.archive_uuid, ae.archive_uuid));
    if (ae.last_global_frame_seq_num + 1 != state.expected_global_frame_seq_num)
        throw std::runtime_error(format(
            "archive end frame seq mismatch: declared {} expected {}",
            ae.last_global_frame_seq_num,
            state.expected_global_frame_seq_num - 1));
}

} // namespace neotape
```

- [ ] **Step 3: Add helper object to Makefile**

Add near other object variables:

```make
RESTORE_VALIDATION_OBJ = src/neotape_restore_validation.o
```

Add `$(RESTORE_VALIDATION_OBJ)` to `bin/neotape`, `bin/neotape-cat-volumes`, and `bin/test_restore_validation` link lines.

- [ ] **Step 4: Replace test file with helper tests**

Replace `tests/test_restore_validation.cpp` with:

```cpp
#include "neotape/restore_validation.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool cond, const std::string &msg) {
    if (!cond) {
        std::cerr << "test_restore_validation: " << msg << "\n";
        std::exit(1);
    }
}

neotape::VolumeHeader volume(std::string uuid, uint64_t seq) {
    neotape::VolumeHeader vh;
    vh.volume_block_size = 4096;
    vh.archive_uuid = std::move(uuid);
    vh.archive_name = "restore-test";
    vh.volume_seq_num = seq;
    vh.payload_profile = neotape::PayloadProfile::pax;
    vh.volume_write_at_utc = "2026-05-25T00:00:00Z";
    return vh;
}

neotape::FrameHeader frame(std::string uuid, uint64_t volume_seq,
                           uint64_t global_seq, uint64_t slice_seq) {
    neotape::FrameHeader fh;
    fh.volume_block_size = 4096;
    fh.archive_uuid = std::move(uuid);
    fh.archive_name = "restore-test";
    fh.volume_seq_num = volume_seq;
    fh.logical_slice_seq_num = slice_seq;
    fh.global_frame_seq_num = global_seq;
    fh.frame_seq_num_within_slice = 1;
    fh.flags = neotape::frame_flag_start;
    return fh;
}

bool throws_with(const std::string &needle, auto fn) {
    try {
        fn();
    } catch (const std::exception &e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

void test_wrong_archive_rejected() {
    neotape::RestoreValidationState state;
    neotape::accept_restore_volume_header(
        volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1), state);
    require(throws_with("archive uuid mismatch", [&] {
                neotape::accept_restore_volume_header(
                    volume("ffffffff-bbbb-cccc-dddd-eeeeeeeeeeee", 2), state);
            }),
            "wrong archive volume rejected");
}

void test_wrong_volume_sequence_rejected() {
    neotape::RestoreValidationState state;
    neotape::accept_restore_volume_header(
        volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1), state);
    require(throws_with("volume sequence mismatch", [&] {
                neotape::accept_restore_volume_header(
                    volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 3), state);
            }),
            "wrong volume sequence rejected");
}

void test_frame_sequence_rejected() {
    neotape::RestoreValidationState state;
    neotape::accept_restore_volume_header(
        volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1), state);
    require(throws_with("frame sequence mismatch", [&] {
                neotape::validate_restore_frame_header(
                    frame("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1, 2, 1),
                    state);
            }),
            "frame sequence mismatch rejected");
}

void test_slice_sequence_rejected() {
    neotape::RestoreValidationState state;
    neotape::accept_restore_volume_header(
        volume("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1), state);
    require(throws_with("slice sequence mismatch", [&] {
                neotape::validate_restore_frame_header(
                    frame("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 1, 1, 2),
                    state);
            }),
            "slice sequence mismatch rejected");
}

} // namespace

int main() {
    test_wrong_archive_rejected();
    test_wrong_volume_sequence_rejected();
    test_frame_sequence_rejected();
    test_slice_sequence_rejected();
    return 0;
}
```

- [ ] **Step 5: Run helper tests**

Run:

```bash
make bin/test_restore_validation && bin/test_restore_validation
```

Expected: exits `0`.

### Task 6: Implement Restore Identity And Clean Boundary Prompt

**Files:**
- Modify: `include/neotape/restore_validation.hpp`
- Modify: `src/neotape_restore_validation.cpp`
- Modify: `src/neotape_cat_volumes.cpp:250-599`
- Create: `tests/smoke_restore_validation.sh`
- Modify: `Makefile:97-102`

- [ ] **Step 1: Include restore validation helpers**

At the top of `src/neotape_cat_volumes.cpp`, add:

```cpp
#include "neotape/restore_validation.hpp"
```

- [ ] **Step 2: Replace duplicate state fields with validation state**

In `src/neotape_cat_volumes.cpp`, extend `VolumeReadState`:

```cpp
    std::string archive_uuid;
    std::string archive_name;
    neotape::PayloadProfile payload_profile = neotape::PayloadProfile::raw;
    uint64_t expected_volume_seq_num = 1;
    uint64_t current_volume_seq_num = 0;
    uint32_t volume_block_size = 0;
    bool identity_established = false;
```

Then replace those duplicate fields with:

```cpp
    neotape::RestoreValidationState validation;
```

- [ ] **Step 3: Remove file-local identity validation helper from this task**

Use `neotape::accept_restore_volume_header()` from `src/neotape_restore_validation.cpp`; do not duplicate the helper in `src/neotape_cat_volumes.cpp`.

- [ ] **Step 4: Validate volume headers in `process_volume()`**

Inside the record loop, immediately after parsing the fixed header, add:

```cpp
            if (parsed.volume) {
                neotape::accept_restore_volume_header(*parsed.volume,
                                                       rs.validation);
                continue;
            }
```

- [ ] **Step 5: Validate frame headers through helper**

Inside `if (parsed.frame)`, before slice-content handling, add:

```cpp
                neotape::validate_restore_frame_header(f, rs.validation);
```

- [ ] **Step 6: Keep BLAKE3 payload verification and then note accepted frames**

After the existing slice payload/hash logic accepts a frame, replace direct increments of `rs.expected_global_frame_seq_num`, `rs.expected_logical_slice_seq_num`, and `rs.slice_open` with updates to `rs.validation` or keep BLAKE3-only state in `VolumeReadState` and mirror sequence fields in `rs.validation`.

The final accepted-frame update must call:

```cpp
                neotape::note_restore_frame_accepted(f, rs.validation);
```

- [ ] **Step 7: Validate Archive End through helper**

Inside `if (parsed.archive_end)`, replace duplicate Archive End sequence checks with:

```cpp
                neotape::validate_restore_archive_end(ae, rs.validation);
```

Keep any BLAKE3/slice-open checks that are not represented in the helper.

- [ ] **Step 8: Treat clean missing Archive End as promptable**

In `run_tape_device_restore()`, replace the single-volume logic with a loop:

```cpp
    bool found_archive_end = false;
    while (true) {
        neotape::TapeDeviceVolumeReader vol(device);
        found_archive_end = process_volume(vol, rs);
        if (found_archive_end)
            break;
        if (rs.validation.slice_open)
            fail("archive incomplete: volume ended with open slice");

        neotape::require_prompt_allowed(opts.control);
        neotape::VolumePromptRequest req;
        req.archive_uuid = rs.validation.archive_uuid;
        req.expected_volume = rs.validation.expected_volume_seq_num;
        req.current_locator = neotape::Locator{"tape", device.device_path()};
        req.write_mode = false;
        auto result = neotape::prompt_for_volume_change(req);
        if (result.choice == neotape::VolumePromptChoice::abort)
            fail("volume change aborted by user");
        if (result.choice == neotape::VolumePromptChoice::change_locator) {
            if (!result.replacement_locator || result.replacement_locator->kind != "tape")
                fail("replacement locator must be tape:<device>");
            device = mt::TapeDevice(result.replacement_locator->locator, false);
        }
    }
```

If assigning `TapeDevice` is awkward in this function because it receives a reference, change `run_tape_device_restore()` to accept an initial locator string and own the `TapeDevice` inside the function.

- [ ] **Step 9: Preserve stdout cleanliness**

Confirm all new diagnostics use `std::cerr` or `fail()`, not `std::cout`, except normal list output.

- [ ] **Step 10: Build**

Run:

```bash
make bin/neotape
```

Expected: build succeeds.

### Task 7: Add Restore Validation Smoke Tests

**Files:**
- Create: `tests/smoke_restore_validation.sh`
- Modify: `Makefile:97-102`

- [ ] **Step 1: Create a shell smoke test for wrong archive rejection**

Create `tests/smoke_restore_validation.sh`:

```sh
#!/bin/sh
set -eu

tmp="${TMPDIR:-/tmp}/neotape-restore-validation-$$"
trap 'rm -rf "$tmp"' EXIT INT TERM

mkdir -p "$tmp/src-a" "$tmp/src-b" "$tmp/out"
printf 'archive-a\n' > "$tmp/src-a/file.txt"
printf 'archive-b\n' > "$tmp/src-b/file.txt"

bin/neotape init "spool:$tmp/a.spool" --label A --virtual-tape-size 64M >/dev/null 2>&1
bin/neotape init "spool:$tmp/b.spool" --label B --virtual-tape-size 64M >/dev/null 2>&1
bin/neotape backup --target "spool:$tmp/a.spool" -C "$tmp" src-a --name A >/dev/null 2>&1
bin/neotape backup --target "spool:$tmp/b.spool" -C "$tmp" src-b --name B >/dev/null 2>&1

mkdir -p "$tmp/mixed.spool"
cp "$tmp/a.spool"/tape-file-000000.medium-header.nts "$tmp/mixed.spool"/
cp "$tmp/a.spool"/tape-file-000001.volume-header.nts "$tmp/mixed.spool"/
cp "$tmp/b.spool"/tape-file-000001.volume-header.nts "$tmp/mixed.spool"/tape-file-000002.volume-header.nts

if bin/neotape restore --source "spool:$tmp/mixed.spool" --output "$tmp/out.pax" --control=none >"$tmp/stdout" 2>"$tmp/stderr"; then
    echo "expected restore to reject wrong archive volume" >&2
    exit 1
fi

if ! grep -q "archive uuid mismatch" "$tmp/stderr"; then
    echo "expected archive uuid mismatch diagnostic" >&2
    cat "$tmp/stderr" >&2
    exit 1
fi
```

If the project policy avoids `grep` in scripts, use shell `case` around captured stderr content instead. Since this is a repository shell test, `grep` is acceptable if existing smoke tests use it.

- [ ] **Step 2: Make the script executable**

Run:

```bash
chmod +x tests/smoke_restore_validation.sh
```

Expected: exit code `0`.

- [ ] **Step 3: Add smoke test to Makefile**

Update `test` target:

```make
	sh tests/smoke_restore_validation.sh
```

Place it after `$(BINDIR)/test_restore_validation` and before the existing pax smoke tests.

- [ ] **Step 4: Run the smoke test**

Run:

```bash
make bin/neotape && sh tests/smoke_restore_validation.sh
```

Expected: exits `0`.

### Task 8: Wire Reader To Helper Sequence Diagnostics

**Files:**
- Modify: `src/neotape_cat_volumes.cpp:261-355`
- Modify: `tests/smoke_restore_validation.sh`

- [ ] **Step 1: Confirm sequence diagnostics live in helper tests**

Verify `tests/test_restore_validation.cpp` contains both:

```cpp
require(throws_with("frame sequence mismatch", [&] {
```

and:

```cpp
require(throws_with("slice sequence mismatch", [&] {
```

Expected: both checks are present from Task 5.

- [ ] **Step 2: Remove duplicate sequence checks from reader**

In `src/neotape_cat_volumes.cpp`, remove duplicate direct checks against `rs.expected_global_frame_seq_num` and `rs.expected_logical_slice_seq_num` after `validate_restore_frame_header()` is wired. The helper owns those diagnostics.

- [ ] **Step 3: Run helper and reader smoke tests**

Run:

```bash
make bin/test_restore_validation bin/neotape && bin/test_restore_validation && sh tests/smoke_restore_validation.sh
```

Expected: exits `0`.

- [ ] **Step 4: Run full tests**

Run:

```bash
make test
```

Expected: all tests pass.

### Task 9: Update Real Tape Validation Plan

**Files:**
- Modify: `docs/superpowers/plans/2026-05-25-real-tape-neotape-validation.md`

- [ ] **Step 1: Update expected EOT prompt behavior**

In Task 6 of the real tape plan, change acceptable behavior so the preferred expected result is now the shared prompt with continue/change device/shell/abort options. Non-zero ENOSPC without prompt is no longer acceptable after implementation unless `--control=none` is used.

- [ ] **Step 2: Add wrong-volume manual cases**

Add a short optional section:

```markdown
### Optional Negative Manual Cases

- Insert a tape containing a different archive UUID when restore asks for the next volume. Expected: restore reports expected/actual UUID and prompts again; it must not emit payload from the wrong volume.
- Insert a tape containing volume 2 for another archive instance. Expected: restore rejects it even if `volume_seq_num == 2`, because archive UUID differs.
- Use `--control=none` for the same cases. Expected: restore exits non-zero without prompting.
```

- [ ] **Step 3: Verify the plan references partition 0 and 8 GiB slices**

Run:

```bash
rg "setpartition 0|--slice-size 8G|wrong archive|volume_seq_num" docs/superpowers/plans/2026-05-25-real-tape-neotape-validation.md
```

Expected: output includes partition 0, 8G slice plan, and wrong-volume negative cases.

### Task 10: Final Verification

**Files:**
- Verify all changed files.

- [ ] **Step 1: Build everything**

Run:

```bash
make -j "$(nproc)"
```

Expected: exit code `0`.

- [ ] **Step 2: Run full test suite**

Run:

```bash
make test
```

Expected: exit code `0`.

- [ ] **Step 3: Inspect changed files**

Run:

```bash
git diff -- include/neotape/tape_writer.hpp src/neotape_write.cpp src/neotape_tape_writer.cpp src/neotape_cat_volumes.cpp tests/test_tape.cpp tests/test_restore_validation.cpp tests/smoke_restore_validation.sh Makefile docs/superpowers/plans/2026-05-25-real-tape-neotape-validation.md
```

Expected: diff only contains volume-change recovery, identity validation, tests, and manual plan updates.

- [ ] **Step 4: Check worktree status**

Run:

```bash
git status --short
```

Expected: only intended files are modified or added. Existing unrelated dirty submodules or old untracked plan/spec files may remain; do not revert them.

## Self-Review

Spec coverage:

- Writer EOT/short-write behavior maps to Tasks 2-4.
- Writer `--control=none` maps to Tasks 2-3.
- Writer abort/change-device prompt maps to Tasks 3-4.
- Restore identity validation maps to Task 6.
- Wrong archive/wrong volume rejection maps to Tasks 6-7.
- Same volume number from another archive instance is rejected by archive UUID validation in Task 6.
- Frame/slice mismatch hard failure maps to Task 8.
- Manual `/dev/nst0` validation updates map to Task 9.

Placeholder scan:

- No task contains placeholder instructions.
- Restore validation helper visibility is fixed by `include/neotape/restore_validation.hpp`, so sequence mismatch tests are direct C++ tests rather than binary mutation tests.

Type consistency:

- `TapeWriterOptions::control` uses existing `neotape::ControlPolicy`.
- Prompt flow uses existing `neotape::VolumePromptRequest`, `VolumePromptResult`, and `VolumePromptChoice`.
- Restore validation uses existing `neotape::VolumeHeader`, `FrameHeader`, and `PayloadProfile`.
