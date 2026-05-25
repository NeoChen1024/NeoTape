# NeoTape Inspect Tape Locator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `neotape-inspect` accept `tape:<device>` while preserving legacy positional spool directory support.

**Architecture:** Parse the positional argument as a NeoTape locator only when it contains `:`, otherwise treat it as a legacy spool directory. Reuse the existing inspection validation functions for tape records by reading them through `mt::TapeDevice` and `neotape::TapeDeviceVolumeReader`.

**Tech Stack:** C++20, `getopt_long`, existing `neotape::parse_locator`, `mt::TapeDevice`, `neotape::TapeDeviceVolumeReader`, Makefile tests.

---

## File Structure

- Modify `src/neotape_inspect.cpp`: add locator parsing, keep legacy spool behavior, add tape inspection path.
- No generated files.

## Task 1: Parse Locator and Preserve Legacy Spool Path

**Files:**
- Modify: `src/neotape_inspect.cpp:1-30`
- Modify: `src/neotape_inspect.cpp:47-69`

- [ ] **Step 1: Run the red parser check**

Run before implementation:

```sh
make bin/neotape-inspect && bin/neotape-inspect tape:/dev/null
```

Expected before implementation: `neotape-inspect: tape:/dev/null is not a directory`, proving `tape:` is treated as a spool path.

- [ ] **Step 2: Add required includes**

At the top of `src/neotape_inspect.cpp`, add:

```cpp
#include "neotape/cli.hpp"
#include "neotape/reader.hpp"
#include "neotape/tape.hpp"
```

- [ ] **Step 3: Change options to hold a locator**

Replace:

```cpp
struct Options {
    fs::path spool_dir;
};
```

with:

```cpp
struct Options {
    neotape::Locator source;
};
```

- [ ] **Step 4: Update usage text**

Change usage to:

```cpp
void usage(const char *prog) {
    std::cerr << format("usage: {} <spool-dir|spool:<dir>|tape:<device>>\n", prog);
}
```

- [ ] **Step 5: Parse positional argument as locator when applicable**

Replace the return at the end of `parse_args()` with:

```cpp
string source = argv[optind++];
if (optind != argc) {
    usage(argv[0]);
    std::exit(2);
}
if (source.find(':') == string::npos)
    return Options{.source = neotape::Locator{"spool", source}};

auto locator = neotape::parse_locator(source);
if (locator.kind != "spool" && locator.kind != "tape")
    fail("inspect source must be spool: or tape:");
return Options{.source = std::move(locator)};
```

## Task 2: Add Tape Inspection Path

**Files:**
- Modify: `src/neotape_inspect.cpp:239-292`
- Modify: `src/neotape_inspect.cpp:296-300`

- [ ] **Step 1: Update spool inspection to use locator**

Change `inspect_spool()` to build a path from `opts.source.locator`:

```cpp
void inspect_spool(const Options &opts) {
    fs::path spool_dir(opts.source.locator);
    require(fs::is_directory(spool_dir),
            format("{} is not a directory", spool_dir.string()));

    InspectState state;
    for (const fs::path &volume_dir : sorted_dirs(spool_dir)) {
        for (const fs::path &file : sorted_files(volume_dir)) {
            inspect_file(file, state);
            if (state.saw_archive_end)
                break;
        }
        if (state.saw_archive_end)
            break;
    }
    require(state.saw_archive_end, "missing Archive End Header");
    std::cout << format("ok: archive {} volumes={} slices={} frames={}\n",
                        state.archive_uuid, state.expected_volume_seq_num - 1,
                        state.expected_slice_seq_num - 1,
                        state.expected_global_frame_seq_num - 1);
}
```

- [ ] **Step 2: Add record-level tape inspection helper**

After `inspect_spool()`, add:

```cpp
void inspect_tape_record(const string &label, InspectState &state,
                         const vector<uint8_t> &record) {
    require(record.size() >= neotape::fixed_header_size,
            format("{}: shorter than fixed header", label));
    neotape::ParsedHeader parsed =
        neotape::parse_fixed_header(record.data(), record.size());
    if (parsed.frame) {
        require(state.volume_block_size != 0,
                format("{}: frame appears before volume header", label));
        inspect_frame(label, state, *parsed.frame, record);
    } else if (parsed.archive_end) {
        inspect_archive_end(label, state, *parsed.archive_end, record.size());
    } else if (parsed.volume) {
        inspect_volume(label, state, *parsed.volume, record.size());
    }
}
```

- [ ] **Step 3: Add tape inspection function**

After `inspect_tape_record()`, add:

```cpp
void inspect_tape(const Options &opts) {
    mt::TapeDevice dev(opts.source.locator, false);
    neotape::TapeDeviceVolumeReader vol(dev);
    InspectState state;
    inspect_volume("tape:volume-header", state, vol.volume_header(),
                   vol.block_size());

    std::vector<uint8_t> record;
    while (vol.next_file()) {
        while (vol.read_record(record)) {
            inspect_tape_record(format("tape:file-{}", vol.tape_file_num()),
                                state, record);
            if (state.saw_archive_end)
                break;
        }
        if (state.saw_archive_end)
            break;
    }

    require(state.saw_archive_end, "missing Archive End Header");
    std::cout << format("ok: archive {} volumes={} slices={} frames={}\n",
                        state.archive_uuid, state.expected_volume_seq_num - 1,
                        state.expected_slice_seq_num - 1,
                        state.expected_global_frame_seq_num - 1);
}
```

- [ ] **Step 4: Dispatch main by locator kind**

Change `main()` to:

```cpp
int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        if (opts.source.kind == "tape")
            inspect_tape(opts);
        else
            inspect_spool(opts);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
```

- [ ] **Step 5: Verify red check changed behavior**

Run:

```sh
make bin/neotape-inspect && bin/neotape-inspect tape:/dev/null
```

Expected after implementation: no `is not a directory`; it should fail because `/dev/null` is not a tape device.

## Task 3: Verify Legacy Spool and Full Suite

**Files:**
- Verify: `src/neotape_inspect.cpp`

- [ ] **Step 1: Run existing spool smoke through inspect if practical**

Run:

```sh
make bin/neotape-inspect bin/neotape && sh tests/smoke_pax_backup_restore.sh
```

Expected: exits 0. This verifies normal spool creation/restoration still works; `neotape-inspect` build verifies link dependencies.

- [ ] **Step 2: Run full verification**

Run:

```sh
make -j "$(nproc)" && make test
```

Expected: exits 0; `bin/test_tape` reports `0 failure(s)`.

- [ ] **Step 3: Inspect relevant diff**

Run:

```sh
git diff -- src/neotape_inspect.cpp docs/superpowers/plans/2026-05-25-neotape-inspect-tape-locator.md
```

Expected: diff is limited to inspect locator/tape support and this plan.

---

## Self-Review

- Spec coverage: plan implements `spool:<dir>`, legacy bare spool dir, and `tape:<device>` for `neotape-inspect`.
- Placeholder scan: no TBD/TODO placeholders remain.
- Type consistency: uses existing `neotape::Locator`, `mt::TapeDevice`, `neotape::TapeDeviceVolumeReader`, and existing inspect validation helpers.
