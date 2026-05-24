# Tape Device Volume Reader Implementation Plan

Archived note: this plan was superseded after `spool:` became the file-backed
CLI backend using the single-root `.nts` spool layout. The old public
`tape:<dir>` restore fallback and `test_file_backed_tape` smoke target were
removed; public `tape:` locators now refer to real tape devices only.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let `neotape restore --source tape:<locator>` consume the same tape abstraction used by `SpoolTapeDevice`, enabling hardware-free tape restore tests before real LTO validation.

**Architecture:** Add a `TapeDeviceVolumeReader` adapter that implements `neotape::VirtualTapeReader` over `mt::TapeDevice`. Reuse the existing `process_volume()` reader validation/output logic in `src/neotape_cat_volumes.cpp` so spool and tape sources share frame verification and payload emission.

**Tech Stack:** C++20, GNU Make, `mt::TapeDevice`, `mt::SpoolTapeDevice`, existing NeoTape reader interfaces, shell smoke tests with `bsdtar`.

---

## Scope Check

This plan implements hardware-free `restore --source tape:` over file-backed `SpoolTapeDevice` locators. It does not require real LTO hardware, interactive volume-change prompts, or multi-cartridge restore.

## File Structure

- Modify `include/neotape/reader.hpp`: declare `TapeDeviceVolumeReader` if it belongs with existing reader abstractions.
- Modify `src/neotape_reader.cpp`: implement reading fixed-size records from an `mt::TapeDevice`, spacing filemarks between tape files.
- Modify `src/neotape_cat_volumes.cpp`: parse `tape:` restore source and instantiate the tape reader path; keep `read` raw command spool-only for now unless the test proves commonization is trivial.
- Modify `tests/smoke_file_backed_tape_roundtrip.sh`: create a file-backed tape archive and restore it through `neotape restore --source tape:<dir>`.
- Modify `Makefile`: ensure the smoke target links new dependencies.

## Task 1: Failing CLI Restore Test

**Files:**
- Modify: `tests/smoke_file_backed_tape_roundtrip.sh`

- [ ] **Step 1: Extend the smoke test**

Make the smoke create a source tree, write a PAX tape archive using a file-backed `SpoolTapeDevice` helper path, then run:

```sh
bin/neotape restore --source "tape:$tape_root" --output "$archive"
bsdtar -xpf "$archive" -C "$out"
cmp "$root/src/file.txt" "$out/src/file.txt"
```

- [ ] **Step 2: Run failing test**

Run: `make test_file_backed_tape`

Expected: FAIL because `restore` currently rejects non-`spool:` sources.

## Task 2: Tape Device Reader Adapter

**Files:**
- Modify: `include/neotape/reader.hpp`
- Modify: `src/neotape_reader.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Add reader declaration**

Add a `TapeDeviceVolumeReader` class that derives from `VirtualTapeReader` and owns or references an `mt::TapeDevice`.

- [ ] **Step 2: Implement record iteration**

Read the first fixed-size record, parse its Volume Header to determine `volume_block_size`, then return records from the current tape file. When the file ends, `next_file()` advances one filemark with `space_fwd_filemark()` and continues until Archive End or device end.

- [ ] **Step 3: Build**

Run: `make bin/neotape`

Expected: build succeeds.

## Task 3: Wire `restore --source tape:`

**Files:**
- Modify: `src/neotape_cat_volumes.cpp`

- [ ] **Step 1: Split restore source dispatch**

Keep `parse_raw_read_args()` for shared `--source`, `--output`, `--archive`, and `--control` parsing, but allow `tape:` for `restore`.

- [ ] **Step 2: Run tape restore path**

For `restore --source tape:<locator>`, create an `mt::SpoolTapeDevice` when `<locator>` is an existing directory; otherwise create a real `mt::TapeDevice`. Feed it into `TapeDeviceVolumeReader` and call the same `process_volume()` loop used for spool.

- [ ] **Step 3: Run smoke**

Run: `make test_file_backed_tape`

Expected: PASS and extracted file matches.

## Verification

- [ ] **Step 1: Full verification**

Run: `make -j "$(nproc)" && make test && make test_tape_backup_wiring && make test_file_backed_tape`

Expected: all commands exit 0.

## Self-Review

- Keeps real LTO validation out of scope.
- Reuses reader validation instead of duplicating frame checks.
- Preserves spool/tape backend interchangeability through `TapeDevice`/`SpoolTapeDevice`.
