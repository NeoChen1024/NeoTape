# NeoTape List Tape Source Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `tape:` locator support to `neotape list --source` while preserving existing spool list output.

**Architecture:** Reuse the existing `mt::TapeDevice` and `mt::nav::TapeNavigator::scan_archive_instances()` path to enumerate archive boundaries on physical tape. Convert each `ArchiveBoundary` to the existing `ArchiveEntry` structure and feed the existing human/JSON printers.

**Tech Stack:** C++20, `getopt_long`, existing tape navigator, existing Makefile tests and shell smoke tests.

---

## File Structure

- Modify `src/neotape_cat_volumes.cpp`: accept `tape:` in `parse_list_args()`, add a tape archive collector, and dispatch list mode by locator kind.
- Modify `tests/smoke_tape_backup_wiring.sh`: add spool-backed CLI coverage only if needed; real tape listing cannot run in CI. The strongest automated coverage available here is parser/link/build coverage plus existing `test_tape` navigator tests.

## Task 1: Add Tape List Path

**Files:**
- Modify: `src/neotape_cat_volumes.cpp:1-10`
- Modify: `src/neotape_cat_volumes.cpp:401-471`
- Modify: `src/neotape_cat_volumes.cpp:478-511`
- Modify: `src/neotape_cat_volumes.cpp:639-646`

- [ ] **Step 1: Write the failing CLI/parser check**

Run before implementation:

```sh
make bin/neotape && bin/neotape list --source tape:/dev/null --json
```

Expected before implementation: `neotape list: list currently supports spool: sources`. `/dev/null` is acceptable because the test is only proving parser-level rejection before the change.

- [ ] **Step 2: Include the tape navigator header**

Add this include near the other tape includes:

```cpp
#include "neotape/tape_navigator.hpp"
```

- [ ] **Step 3: Add tape boundary conversion**

After `collect_archives(SpoolOrchestrator &orch)`, add:

```cpp
std::vector<ArchiveEntry>
collect_archives_from_tape_boundaries(const std::vector<mt::nav::ArchiveBoundary> &boundaries) {
    std::vector<ArchiveEntry> entries;
    entries.reserve(boundaries.size());
    for (const auto &boundary : boundaries) {
        ArchiveEntry entry;
        entry.uuid = boundary.volume_header.archive_uuid;
        entry.name = boundary.volume_header.archive_name;
        entry.profile = neotape::payload_profile_name(
            boundary.volume_header.payload_profile);
        entry.status = "clean";
        entry.block_size = boundary.volume_header.volume_block_size;
        entry.total_frames = boundary.end_header.last_global_frame_seq_num;
        entry.volume_count = 1;
        entries.push_back(std::move(entry));
    }
    return entries;
}
```

- [ ] **Step 4: Accept tape locators in list args**

Change the source kind validation:

```cpp
if (opts.source.kind != "spool" && opts.source.kind != "tape")
    fail("list source must be spool: or tape:");
```

- [ ] **Step 5: Dispatch list by locator kind**

In `neotape_list_main()`, replace the unconditional spool orchestration with:

```cpp
std::vector<ArchiveEntry> entries;
if (opts.source.kind == "spool") {
    SpoolOrchestrator orch(opts.source.locator);
    entries = collect_archives(orch);
} else {
    mt::TapeDevice dev(opts.source.locator, false);
    mt::nav::TapeNavigator nav(dev);
    entries = collect_archives_from_tape_boundaries(
        nav.scan_archive_instances());
}

if (opts.json)
    print_archives_json(entries);
else
    print_archives_human(entries);
```

This preserves existing output format and lets the local `TapeDevice` destructor close the fd after listing.

- [ ] **Step 6: Verify parser rejection is gone**

Run:

```sh
make bin/neotape && bin/neotape list --source tape:/dev/null --json
```

Expected after implementation: failure should no longer say `list currently supports spool: sources`; it should fail opening or using `/dev/null` as a tape device.

## Task 2: Verify Existing Spool List Still Works

**Files:**
- Verify: `tests/smoke_pax_backup_restore.sh`
- Verify: `src/neotape_cat_volumes.cpp`

- [ ] **Step 1: Run focused smoke test**

Run:

```sh
make bin/neotape && sh tests/smoke_pax_backup_restore.sh
```

Expected: exits 0, including existing `bin/neotape list --source "spool:$spool" --json` coverage.

- [ ] **Step 2: Run full verification**

Run:

```sh
make -j "$(nproc)" && make test
```

Expected: exits 0; `bin/test_tape` reports `0 failure(s)`.

- [ ] **Step 3: Inspect relevant diff**

Run:

```sh
git diff -- src/neotape_cat_volumes.cpp docs/superpowers/plans/2026-05-25-neotape-list-tape-source.md
```

Expected: diff is limited to list tape support and this plan.

---

## Self-Review

- Spec coverage: plan accepts `tape:` for `neotape list --source`, reuses `TapeNavigator`, preserves spool output, and closes the tape fd via local RAII lifetime.
- Placeholder scan: no TBD/TODO placeholders remain.
- Type consistency: `ArchiveBoundary`, `ArchiveEntry`, and `TapeNavigator` names match existing code.
