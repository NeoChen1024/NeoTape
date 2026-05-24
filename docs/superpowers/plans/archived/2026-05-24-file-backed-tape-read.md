# File-Backed Tape Read Implementation Plan

Archived note: this plan was superseded after `spool:` became the file-backed
CLI backend using the single-root `.nts` spool layout. The old public
`tape:<dir>`/`test_file_backed_tape` direction was removed; public `tape:`
locators now refer to real tape devices only.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the tape abstraction can write and read a PAX backup through a file-backed `SpoolTapeDevice` without real LTO hardware.

**Architecture:** Extend the existing tape/spool abstraction by making `SpoolTapeDevice` readable enough for record iteration, then add a reader path that can consume the single-root tape-file layout emitted by `SpoolTapeDevice`. Keep the existing archive verification logic intact and avoid duplicating frame validation.

**Tech Stack:** C++20, GNU Make, existing `TapeDevice`/`SpoolTapeDevice`, existing NeoTape reader and format helpers, shell smoke tests with `bsdtar`.

---

## Scope Check

This plan covers hardware-free tape read/write round trip. It does not require or validate real `/dev/nst0` hardware, multi-volume physical prompt flow, or tape EOT recovery.

## File Structure

- Modify `include/neotape/tape.hpp`: expose minimal readable behavior already represented by `SpoolTapeDevice` methods.
- Modify `src/neotape_tape.cpp`: implement read-mode filemark spacing for `SpoolTapeDevice` so it can iterate single-root tape files.
- Modify `tests/test_tape.cpp`: add a unit-level round trip that writes with `write_tape_archive_from_chunks_to_device()` and reads records back through `SpoolTapeDevice`.
- Modify `tests/smoke_tape_backup_wiring.sh`: keep CLI wiring smoke focused on the absence of the old CLI block.
- Modify `Makefile`: ensure `make test` includes the file-backed tape tests.

## Task 1: Read Back Records From File-Backed Tape

**Files:**
- Modify: `tests/test_tape.cpp`
- Modify: `src/neotape_tape.cpp`

- [ ] **Step 1: Write failing test**

In `tests/test_tape.cpp`, after the callback writer test, open the same root as `mt::SpoolTapeDevice reader(root, false)`, read one fixed-size record from `reader.fd()`, parse it as a Volume Header, call `reader.space_fwd_filemark()`, read the next record and parse it as a Frame Header, call `reader.space_fwd_filemark()`, then read the Archive End Header.

- [ ] **Step 2: Run the failing test**

Run: `make bin/test_tape && bin/test_tape`

Expected: FAIL because `SpoolTapeDevice::do_mtop()` currently throws `ENOTSUP` for read-mode filemark spacing.

- [ ] **Step 3: Implement read-mode filemark spacing**

In `src/neotape_tape.cpp`, make `SpoolTapeDevice::do_mtop(MTFSF, 1)` and `MTFSFM, 1` close the current file, advance to the next sorted spool file, and open it read-only. Return an EOF-like status when no next file exists by closing the fd and setting `spool_fd_ = -1`.

- [ ] **Step 4: Re-run test**

Run: `make bin/test_tape && bin/test_tape`

Expected: PASS, including parsed Volume Header, Frame Header, and Archive End Header from the file-backed tape output.

## Task 2: Add Hardware-Free PAX Tape Round Trip Smoke

**Files:**
- Create: `tests/smoke_file_backed_tape_roundtrip.sh`
- Modify: `Makefile`

- [ ] **Step 1: Write smoke shell**

Create `tests/smoke_file_backed_tape_roundtrip.sh` to run `bin/test_tape` and assert it exits 0. Keep this shell as a named integration target so future work can grow it into CLI-level `restore --source tape:` without hiding it inside unit output.

- [ ] **Step 2: Add Makefile target**

Add:

```make
test_file_backed_tape: $(BINDIR)/test_tape
	sh tests/smoke_file_backed_tape_roundtrip.sh
```

Include it in `make test` after `bin/test_tape` is stable.

- [ ] **Step 3: Run smoke**

Run: `make test_file_backed_tape`

Expected: PASS.

## Task 3: CLI Restore Design Gate

**Files:**
- Modify later: `src/neotape_cat_volumes.cpp`

- [ ] **Step 1: Stop before CLI restore if reader abstraction is insufficient**

If `SpoolTapeDevice` can read records but `neotape restore --source tape:` still needs a full `VirtualTapeReader` adapter, stop after Task 2 and report that the next unit should be a `TapeDeviceVolumeReader`. Do not bolt CLI restore directly onto ad-hoc file reads.

## Verification

- [ ] **Step 1: Full verification**

Run: `make -j "$(nproc)" && make test && make test_tape_backup_wiring && make test_file_backed_tape`

Expected: all commands exit 0.

## Self-Review

- The plan validates the spool-as-tape abstraction before real hardware.
- It avoids introducing temp PAX streams.
- It does not overreach into real LTO validation or full interactive volume-change behavior.
