# Spool Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace ad-hoc spool directory handling with a NeoTape-header-aware virtual tape backend that writes the new single-directory `.nts` spool layout.

**Architecture:** Add a `SpoolTapeDevice` implementation alongside `TapeDevice` in the tape abstraction layer. It exposes tape-like filemark, rewind, EOD, read, and write operations while naming spool files by parsing the first NeoTape record header in each tape file. Existing writer/reader tools are then moved toward this backend instead of per-tool directory logic.

**Tech Stack:** C++20, GNU Make, existing NeoTape format parser/serializer, filesystem, stdio/POSIX read/write API.

---

### Task 1: Add Failing Spool Backend Tests

**Files:**
- Modify: `tests/test_tape.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Add tests proving the desired spool backend API**

Add tests that construct `mt::SpoolTapeDevice`, write serialized NeoTape records through `write(2)` on `fd()`, call `write_filemark()`, and assert that files appear as `tape-file-000000.volume-header.nts`, `tape-file-000001.slice-000001.nts`, and `tape-file-000002.archive-end.nts` in one root directory.

- [ ] **Step 2: Run failing test**

Run: `make test`

Expected: compile failure because `mt::SpoolTapeDevice` does not exist yet.

### Task 2: Implement Minimal SpoolTapeDevice

**Files:**
- Modify: `include/neotape/tape.hpp`
- Modify: `src/neotape_tape.cpp`

- [ ] **Step 1: Declare `mt::SpoolTapeDevice`**

Add a concrete subclass of `TapeDevice` with constructor `SpoolTapeDevice(const std::filesystem::path &root, bool read_write = false)` and override `fd()`, `do_mtop()`, `do_tell()`, and `do_status()`.

- [ ] **Step 2: Implement write-side filemark semantics**

Use an internal temporary file descriptor for the current tape file. On `MTWEOF`, read the first record, parse `neotape::parse_fixed_header()`, derive the `.nts` file name, close the temp file, and rename it into the spool root.

- [ ] **Step 3: Run test to verify green**

Run: `make test`

Expected: all tests pass.

### Task 3: Switch Spool Writer To New Layout

**Files:**
- Modify: `src/neotape_write.cpp`

- [ ] **Step 1: Add failing integration check**

Run the writer against a temp spool root and assert no `tape-*` directory and no `.ntf` file is produced.

- [ ] **Step 2: Replace writer spool paths**

Make `output_dir` the spool root, start `tape_file_num` at zero, emit `.nts`, and remove `tape-<seq>` directory rollover.

- [ ] **Step 3: Run integration check and build**

Run: `make -j "$(nproc)" && make test`

Expected: build and tests pass.

### Task 4: Update Reader And Tools

**Files:**
- Modify: `src/neotape_reader.cpp`
- Modify: `include/neotape/reader.hpp`
- Modify: `src/neotape_cat_volumes.cpp`
- Modify: `src/neotape_inspect.cpp`

- [ ] **Step 1: Add failing readback check**

Generate a spool with `neotape-write`, inspect it, and extract it with `neotape-cat-volumes`.

- [ ] **Step 2: Make scanners accept only single-root `.nts`**

Replace old directory iteration with numeric sorting of `tape-file-<num>.*.nts` directly under the spool root.

- [ ] **Step 3: Run verification**

Run: `make -j "$(nproc)" && make test`, then run the generated-spool inspect/extract round trip.

Expected: all pass.

### Task 5: Simplify Density Lookup

**Files:**
- Modify: `src/neotape_tape.cpp`

- [ ] **Step 1: Keep existing density test red/green coverage**

The existing `density code 0x58 -> LTO-5 Ultrium` test covers behavior.

- [ ] **Step 2: Replace struct array loop with map lookup**

Use `static const std::map<int, std::string>` in `density_name_for_code()` and return the mapped string or `unknown`.

- [ ] **Step 3: Run verification**

Run: `make test`

Expected: all tests pass.
