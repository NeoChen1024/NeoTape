# Read Restore Volume Changer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore shared volume changer behavior for `neotape read` and `neotape restore` across tape and spool sources, while preserving writer-side changer behavior.

**Architecture:** Keep the implementation localized to `src/neotape_read_cmd.cpp` and existing shell smoke tests. Add a small reader-side helper that prompts through `prompt_for_volume_change()`, validates replacement locator kind, and returns the next locator. Use one archive-level `VolumeReadState` across all volumes so existing restore validation continues to enforce archive identity, volume order, frame order, slice hashes, and archive end semantics.

**Tech Stack:** C++20, GNU Make, shell smoke tests, existing NeoTape CLI binaries.

---

## File Structure

- Modify `src/neotape_read_cmd.cpp`: allow `neotape read` to accept `tape:` sources, route both `read` and `restore` through source-kind-specific orchestration, and use a shared reader-side volume changer helper.
- Modify `tests/smoke_pax_backup_restore.sh`: add a regression assertion that spool `restore --control=none` reaches the volume-change-required path for an incomplete archive.
- Modify `tests/smoke_tape_backup_wiring.sh`: add a regression assertion that `neotape read --source tape:<path>` reaches the tape backend instead of being rejected at CLI parsing.
- Existing `tests/test_tape.cpp` remains the writer-side coverage for `--control=none` on tape write prompt paths.

### Task 1: Reader CLI Regression Tests

**Files:**
- Modify: `tests/smoke_pax_backup_restore.sh`
- Modify: `tests/smoke_tape_backup_wiring.sh`

- [ ] **Step 1: Add a failing spool restore control test**

In `tests/smoke_pax_backup_restore.sh`, add a separate restore output path and extend cleanup variables near the top:

```sh
missing_restore_out=/tmp/neotape-missing-volume-restore.out
missing_restore_err=/tmp/neotape-missing-volume-restore.err
```

Update the `rm -rf` command so it removes both new paths:

```sh
rm -rf "$root" "$spool" "$archive" "$out" "$list_json" "$planned_spool" \
    "$planned_archive" "$planned_out" "$plan" "$planned_backup_err" "$missing_spool" \
    "$missing_in" "$missing_out" "$missing_err" "$missing_restore_out" \
    "$missing_restore_err"
```

After the existing incomplete-spool `neotape read --control=none` assertion, add:

```sh
if bin/neotape restore --source "spool:$missing_spool" --output "$missing_restore_out" \
    --control=none 2>"$missing_restore_err"; then
    printf 'expected restore with missing continuation volume to fail\n' >&2
    exit 1
fi
grep -q 'volume change required but --control=none is set' "$missing_restore_err"
```

- [ ] **Step 2: Add a failing tape read CLI acceptance test**

In `tests/smoke_tape_backup_wiring.sh`, after the existing tape restore directory test, add:

```sh
if bin/neotape read --source "tape:$dir" --output /tmp/neotape-tape-read-wiring.raw \
    2>"$err"; then
    printf 'expected tape read against a directory to fail\n' >&2
    exit 1
fi

if grep -q 'read currently supports spool' "$err"; then
    printf 'tape read is still blocked at CLI layer\n' >&2
    exit 1
fi

grep -q "$dir" "$err"
```

- [ ] **Step 3: Run focused tests and verify RED**

Run:

```sh
make bin/neotape
sh tests/smoke_pax_backup_restore.sh
```

Expected: `smoke_pax_backup_restore.sh` fails at the new restore assertion because current spool restore reports `archive incomplete: no Archive End Header found` instead of `volume change required but --control=none is set`.

Run:

```sh
sh tests/smoke_tape_backup_wiring.sh
```

Expected: `smoke_tape_backup_wiring.sh` fails at `tape read is still blocked at CLI layer` because current `neotape read` rejects `tape:` before opening the backend.

### Task 2: Shared Reader Volume Changer Helper

**Files:**
- Modify: `src/neotape_read_cmd.cpp`

- [ ] **Step 1: Add the helper above the orchestration functions**

In `src/neotape_read_cmd.cpp`, after `process_volume()` and before the list-archive helpers, add:

```cpp
neotape::Locator request_next_volume(const Options &opts, OutputSink &sink,
                                     const neotape::Locator &current,
                                     const VolumeReadState &rs) {
    sink.flush();
    neotape::require_prompt_allowed(opts.control);

    neotape::VolumePromptRequest req;
    req.archive_uuid = rs.validation.archive_uuid;
    req.expected_volume = rs.validation.expected_volume_seq_num;
    req.current_locator = current;
    req.write_mode = false;

    auto result = neotape::prompt_for_volume_change(req);
    if (result.choice == neotape::VolumePromptChoice::abort)
        fail("volume change aborted by user");
    if (result.choice == neotape::VolumePromptChoice::change_locator) {
        if (!result.replacement_locator)
            fail("replacement locator required");
        if (result.replacement_locator->kind != current.kind)
            fail(format("replacement locator must be {}:<locator>",
                        current.kind));
        return *result.replacement_locator;
    }
    if (result.choice != neotape::VolumePromptChoice::continue_current)
        fail("unsupported volume prompt choice");
    return current;
}
```

- [ ] **Step 2: Compile to catch helper integration errors**

Run:

```sh
make bin/neotape
```

Expected: build succeeds or fails only because the helper is unused. If `-Wunused-function` fails, continue Task 3 before re-running.

### Task 3: Spool Read And Restore Changer Flow

**Files:**
- Modify: `src/neotape_read_cmd.cpp`

- [ ] **Step 1: Replace single-directory spool orchestration with locator loop**

Change `run_cat_volumes(const Options &opts)` so it builds a current `spool:` locator and loops until `process_volume()` returns true. Replace the body with:

```cpp
void run_cat_volumes(const Options &opts) {
    neotape::Locator current{"spool", opts.spool_dir};
    auto sink = create_sink(opts);
    VolumeReadState rs{};
    rs.sink = sink.get();
    uint64_t total_volumes = 0;

    while (true) {
        SpoolOrchestrator orch(current.locator);

        if (opts.list_mode) {
            list_archives(orch);
            return;
        }

        auto vol = orch.next_volume();
        if (!vol)
            fail("no volume found in spool source");
        ++total_volumes;

        bool found_archive_end = process_volume(*vol, rs, vol->volume_header());
        if (found_archive_end)
            break;
        if (rs.validation.slice_open)
            fail("archive incomplete: volume ended with open slice");

        current = request_next_volume(opts, *sink, current, rs);
    }

    sink->flush();

    std::cerr << format(
        "neotape read: ok volumes={} slices={} frames={}\n",
        total_volumes, rs.validation.expected_logical_slice_seq_num - 1,
        rs.validation.expected_global_frame_seq_num - 1);
}
```

- [ ] **Step 2: Run focused spool test and verify GREEN for spool restore**

Run:

```sh
make bin/neotape
sh tests/smoke_pax_backup_restore.sh
```

Expected: the script passes the incomplete-spool `read --control=none` and new `restore --control=none` assertions. If a complete restore smoke fails, keep the same `VolumeReadState` loop and fix only the regression introduced in this task.

### Task 4: Tape Read Changer Flow

**Files:**
- Modify: `src/neotape_read_cmd.cpp`

- [ ] **Step 1: Update raw read parsing to allow tape for `read`**

In `neotape_read_main`, parse with `allow_tape = true` and dispatch by source kind:

```cpp
int neotape_read_main(int argc, char **argv) {
    try {
        auto raw = parse_raw_read_args(argc, argv, true);
        Options opts;
        opts.output = raw.output;
        opts.control = raw.control;
        if (raw.source.kind == "tape") {
            run_tape_device_restore(opts, raw.source.locator);
        } else {
            opts.spool_dir = raw.source.locator;
            run_cat_volumes(opts);
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape read: {}\n", e.what());
        return 1;
    }
}
```

- [ ] **Step 2: Change tape restore loop to use the helper**

Replace `run_tape_device_restore()` with this locator-based loop:

```cpp
void run_tape_device_restore(const Options &opts, const string &locator) {
    neotape::Locator current{"tape", locator};
    auto sink = create_sink(opts);
    VolumeReadState rs{};
    rs.sink = sink.get();
    uint64_t total_volumes = 0;

    while (true) {
        auto device = std::make_unique<mt::TapeDevice>(current.locator, false);
        {
            neotape::TapeDeviceVolumeReader vol(*device);
            ++total_volumes;
            bool found_archive_end = process_volume(vol, rs, vol.volume_header());
            if (found_archive_end)
                break;
            if (rs.validation.slice_open)
                fail("archive incomplete: volume ended with open slice");
        }

        device->close();
        current = request_next_volume(opts, *sink, current, rs);
    }

    sink->flush();
    std::cerr << format(
        "neotape restore: ok volumes={} slices={} frames={}\n",
        total_volumes, rs.validation.expected_logical_slice_seq_num - 1,
        rs.validation.expected_global_frame_seq_num - 1);
}
```

- [ ] **Step 3: Run focused tape wiring test and verify GREEN**

Run:

```sh
make bin/neotape
sh tests/smoke_tape_backup_wiring.sh
```

Expected: the script passes. The new tape read assertion should fail through the tape backend and include the directory path, not `read currently supports spool`.

### Task 5: Full Verification And Commit

**Files:**
- Verify: all modified files

- [ ] **Step 1: Run full test suite**

Run:

```sh
make test
```

Expected: all C++ unit tests and shell smoke tests pass.

- [ ] **Step 2: Check final diff and whitespace**

Run:

```sh
git diff -- src/neotape_read_cmd.cpp tests/smoke_pax_backup_restore.sh tests/smoke_tape_backup_wiring.sh
git diff --check
```

Expected: diff contains only the reader-side changer restoration and smoke tests; `git diff --check` has no output.

- [ ] **Step 3: Commit implementation**

Run:

```sh
git add src/neotape_read_cmd.cpp tests/smoke_pax_backup_restore.sh tests/smoke_tape_backup_wiring.sh
git commit -m "fix: restore reader volume changer support"
```

Expected: commit succeeds.

## Plan Self-Review

- Spec coverage: reader-side tape and spool changer behavior, `--control=none`, stdout cleanliness through sink flushing, replacement locator kind checks, open-slice failure, and writer-side preservation are covered.
- Placeholder scan: no placeholder implementation steps remain.
- Type consistency: helper uses existing `Options`, `OutputSink`, `VolumeReadState`, `neotape::Locator`, and `VolumePromptChoice` names from `src/neotape_read_cmd.cpp` and `include/neotape/cli.hpp`.
