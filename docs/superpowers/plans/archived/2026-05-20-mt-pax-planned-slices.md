# mt-pax Planned Slices Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `mt-pax` into a reusable pax writer library and add planned slice output for debugging.

**Architecture:** Move the existing multi-threaded pax pipeline into `neotape::write_pax`, driven by callbacks for slice lifecycle and byte chunks. `bin/mt-pax` becomes a CLI wrapper that either writes one continuous stream with `-f` or writes one raw pax payload file per planned slice using `--plan` and `--slice-output-prefix`.

**Tech Stack:** C++20, libarchive, bundled BLAKE3, existing `BoundedBuffer`, GNU Make, shell smoke tests.

---

## File Structure

- Create `include/neotape/pax_writer.hpp`: public library types and `write_pax` declaration.
- Create `src/neotape_pax_writer.cpp`: moved mt-pax pipeline, plan parser, planned/unplanned entry dispatch, callback sink, BLAKE3 result computation.
- Replace `src/mt-pax.cpp`: CLI parser and output callback wiring only.
- Modify `Makefile`: add `PAX_WRITER_OBJ` and link it into `bin/mt-pax`.
- Modify `docs/implementation/mt-pax-architecture.md`: record that the implementation is now library-backed and supports planned slice callbacks.

## Implementation Tasks

### Task 1: Add Public Library Header

**Files:**
- Create: `include/neotape/pax_writer.hpp`

- [ ] **Step 1: Add API types**

Create the header with this content:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace neotape {

struct PaxChunk {
    uint64_t slice = 0;
    std::span<const std::byte> bytes;
};

struct PaxWriterOptions {
    std::string output_name = "-";
    std::vector<std::string> sources;
    std::optional<std::filesystem::path> plan_path;
    int verbose = 0;
    bool one_file_system = false;
    std::optional<std::string> chdir_dir;
    std::size_t output_buf_size = 64UL * 1024 * 1024;
    unsigned buffer_percent = 0;
    unsigned io_thread = 1;
};

struct PaxWriterCallbacks {
    std::function<void(uint64_t)> begin_slice;
    std::function<void(PaxChunk)> write_chunk;
    std::function<void(uint64_t)> end_slice;
};

struct PaxWriteResult {
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
    uint64_t walked_entries = 0;
    uint64_t slices = 0;
    std::string blake3_hex;
};

PaxWriteResult write_pax(const PaxWriterOptions &opts,
    PaxWriterCallbacks callbacks);

void ensure_utf8_ctype_locale();

} // namespace neotape
```

- [ ] **Step 2: Build to verify missing implementation fails**

Run: `make -j "$(nproc)"`

Expected: existing tree may still build because the header is not referenced yet. If it fails, the failure must be unrelated to `pax_writer.hpp` contents.

### Task 2: Move Existing mt-pax Pipeline Into Library

**Files:**
- Create: `src/neotape_pax_writer.cpp`
- Replace: `src/mt-pax.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Copy current pipeline into `src/neotape_pax_writer.cpp`**

Start from the current `src/mt-pax.cpp`, then apply these mechanical changes:

```cpp
#include "neotape/pax_writer.hpp"

namespace neotape {

namespace {
// existing helper types and functions, except CLI parsing and main
}

PaxWriteResult write_pax(const PaxWriterOptions &opts,
    PaxWriterCallbacks callbacks) {
    return write_pax_archive_impl(opts, std::move(callbacks));
}

} // namespace neotape
```

Rename the moved internal `Options` struct to use `neotape::PaxWriterOptions`, and rename the existing `write_pax_archive` body to `write_pax_archive_impl`.

- [ ] **Step 2: Replace direct output file ownership with callbacks**

In the output-thread section, remove `FILE *out_file` and `close_file`. Replace `fwrite` with:

```cpp
callbacks.write_chunk(PaxChunk{
    .slice = current_output_slice.load(std::memory_order_relaxed),
    .bytes = std::span<const std::byte>(chunk.data(), chunk.size()),
});
```

Keep BLAKE3 updates and `stats.output_bytes` increments after callback success.

- [ ] **Step 3: Add continuous slice lifecycle**

For unplanned mode, call:

```cpp
callbacks.begin_slice(0);
```

before walking sources and:

```cpp
callbacks.end_slice(0);
```

after the serializer/output pipeline is fully drained.

- [ ] **Step 4: Return `PaxWriteResult`**

At the end of `write_pax_archive_impl`, return:

```cpp
return PaxWriteResult{
    .input_bytes = stats.input_bytes.load(std::memory_order_relaxed),
    .output_bytes = stats.output_bytes.load(std::memory_order_relaxed),
    .walked_entries = stats.walked_entries.load(std::memory_order_relaxed),
    .slices = emitted_slice_count,
    .blake3_hex = hex,
};
```

- [ ] **Step 5: Replace `src/mt-pax.cpp` with CLI wrapper**

The wrapper should include `neotape/pax_writer.hpp`, parse the existing options with `getopt_long`, open output files in callbacks, call `neotape::write_pax`, and print the returned BLAKE3 hash.

- [ ] **Step 6: Update Makefile**

Add:

```make
PAX_WRITER_OBJ = $(BUILDDIR)/neotape_pax_writer.o
```

Change `bin/mt-pax` to depend on and link `$(PAX_WRITER_OBJ)`:

```make
$(BINDIR)/mt-pax : src/mt-pax.cpp $(PAX_WRITER_OBJ) $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(PAX_WRITER_OBJ) $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) -o $@ $(LDLIBS)
```

Add `$(PAX_WRITER_OBJ)` to `clean`.

- [ ] **Step 7: Build**

Run: `make -j "$(nproc)"`

Expected: `bin/mt-pax` links successfully.

### Task 3: Add Plan Parser and Planned Entry Dispatch

**Files:**
- Modify: `src/neotape_pax_writer.cpp`

- [ ] **Step 1: Add plan record types**

Add internal types:

```cpp
struct PlannedEntry {
    uint64_t slice = 0;
    uint64_t file_num = 0;
    char kind = '?';
    uint64_t size = 0;
    string path;
};

struct PlanRecord {
    std::optional<string> chdir_dir;
    std::optional<PlannedEntry> entry;
};
```

- [ ] **Step 2: Parse plan records**

Implement `read_plan_records(const fs::path &path)` by reading the file as bytes, splitting on `\0\n`, accepting `/chdir/<path>` directives, and parsing entry records of the form `/<slice>/<file_num>/<kind>/<size>/<filepath>`.

On invalid input, throw `std::runtime_error(format("{}:{}: invalid plan record", path.string(), record_num))`.

- [ ] **Step 3: Create entries from planned paths**

Implement a helper that creates an `archive_entry *` using `archive_read_disk_entry_from_file`:

```cpp
archive_entry *entry_from_planned_path(archive *disk, const string &path) {
    archive_entry *entry = archive_entry_new();
    if (!entry)
        throw std::runtime_error("cannot allocate entry");
    archive_entry_set_pathname_utf8(entry, path.c_str());
    archive_entry_set_sourcepath(entry, path.c_str());
    int r = archive_read_disk_entry_from_file(disk, entry, -1, nullptr);
    if (r == ARCHIVE_FATAL)
        fail_archive("read filesystem", disk);
    if (r < ARCHIVE_OK)
        warn_archive("read filesystem", disk);
    mark_link_target_as_utf8(entry);
    return entry;
}
```

- [ ] **Step 4: Add planned dispatcher loop**

When `opts.plan_path` has a value, skip `archive_read_disk_open` traversal. Use one `archive_read_disk_new()` object with standard lookup and symlink physical behavior, process records in file order, call `chdir` on directives, and dispatch each planned entry through the same hardlink resolver and `dispatch_entry` function used by unplanned mode.

- [ ] **Step 5: Emit slice boundaries from plan changes**

Before dispatching the first entry for a slice, call `callbacks.begin_slice(slice)`. Before switching to a new slice, wait until the previous slice's queued output is drained, then call `callbacks.end_slice(previous_slice)`.

The minimal implementation can enforce drain by reusing a special control item in the serializer queue or by running each planned slice as one invocation of the pipeline. Prefer a control item if it keeps unplanned behavior unchanged.

- [ ] **Step 6: Build**

Run: `make -j "$(nproc)"`

Expected: `bin/mt-pax` links successfully.

### Task 4: Add Planned Slice CLI Output

**Files:**
- Modify: `src/mt-pax.cpp`

- [ ] **Step 1: Add CLI options**

Add long options:

```cpp
{"plan", required_argument, nullptr, 258},
{"slice-output-prefix", required_argument, nullptr, 259},
```

Store them in wrapper-local variables:

```cpp
std::optional<std::filesystem::path> plan_path;
std::optional<string> slice_output_prefix;
```

- [ ] **Step 2: Validate output modes**

Reject invalid combinations:

```cpp
if (plan_path.has_value() != slice_output_prefix.has_value())
    fail("--plan and --slice-output-prefix must be used together");
if (slice_output_prefix.has_value() && !opts.output.empty())
    fail("-f cannot be used with --slice-output-prefix");
if (!slice_output_prefix.has_value() && opts.output.empty())
    fail("-f is required without --slice-output-prefix");
```

- [ ] **Step 3: Implement slice file callbacks**

Use callbacks that open `<prefix><six(slice)>.pax` on `begin_slice`, write chunks to the current file, and close on `end_slice`.

```cpp
auto slice_path = [&](uint64_t slice) {
    return format("{}{:06}.pax", *slice_output_prefix, slice);
};
```

The callback must not append pax EOA markers.

- [ ] **Step 4: Preserve continuous callbacks**

For normal `-f`, callbacks open one `FILE *` before `write_pax`, ignore slice boundaries, and write every chunk to the selected output.

- [ ] **Step 5: Build**

Run: `make -j "$(nproc)"`

Expected: `bin/mt-pax` links successfully.

### Task 5: Smoke Verify Behavior

**Files:**
- No source changes unless verification exposes a defect.

- [ ] **Step 1: Create a fixture**

Run:

```sh
rm -rf /tmp/opencode/mt-pax-fixture /tmp/opencode/mt-pax-out
mkdir -p /tmp/opencode/mt-pax-fixture/dir /tmp/opencode/mt-pax-out
printf 'alpha\n' > /tmp/opencode/mt-pax-fixture/a.txt
printf 'beta\n' > /tmp/opencode/mt-pax-fixture/dir/b.txt
```

Expected: command exits `0`.

- [ ] **Step 2: Verify unplanned mode**

Run:

```sh
bin/mt-pax -f /tmp/opencode/mt-pax-out/all.pax /tmp/opencode/mt-pax-fixture
bsdtar -tf /tmp/opencode/mt-pax-out/all.pax >/tmp/opencode/mt-pax-out/all.list
```

Expected: `bin/mt-pax` exits `0`; `bsdtar` lists entries or exits successfully despite missing final EOA behavior.

- [ ] **Step 3: Generate a plan**

Run:

```sh
bin/neotape-plan -o /tmp/opencode/mt-pax-out/plan.ntplan --slice-size 1K /tmp/opencode/mt-pax-fixture
```

Expected: plan file exists and has multiple entry records. It may produce one or more slices depending on filesystem block accounting.

- [ ] **Step 4: Verify planned slice files**

Run:

```sh
bin/mt-pax --plan /tmp/opencode/mt-pax-out/plan.ntplan --slice-output-prefix /tmp/opencode/mt-pax-out/slice-
ls /tmp/opencode/mt-pax-out/slice-*.pax
```

Expected: at least `/tmp/opencode/mt-pax-out/slice-000000.pax` exists.

- [ ] **Step 5: Verify concatenated planned stream**

Run:

```sh
sh -c 'cat /tmp/opencode/mt-pax-out/slice-*.pax > /tmp/opencode/mt-pax-out/planned.pax'
bsdtar -tf /tmp/opencode/mt-pax-out/planned.pax >/tmp/opencode/mt-pax-out/planned.list
```

Expected: listed paths match the unplanned archive entries except for ordering differences caused by plan traversal. Missing-EOA warnings are acceptable if `bsdtar` still lists the entries.

### Task 6: Update Documentation

**Files:**
- Modify: `docs/implementation/mt-pax-architecture.md`

- [ ] **Step 1: Add library/refactor note**

Add a short section after the overview:

```markdown
## Library Interface

The implementation lives in `src/neotape_pax_writer.cpp` and is exposed through
`include/neotape/pax_writer.hpp`. `src/mt-pax.cpp` is a CLI wrapper that maps
command-line output modes onto library callbacks.

The library emits slice lifecycle events and pax byte chunks. Unplanned source
walking emits one logical slice, while planned mode consumes `neotape-plan`
metadata and opens/closes slices according to the plan's slice numbers.
```

- [ ] **Step 2: Build after docs update**

Run: `make -j "$(nproc)"`

Expected: build still succeeds.

## Self-Review Checklist

- Spec coverage: library interface, planned metadata consumption, CLI debug slice files, no EOA in slice files, build integration, and smoke verification are covered.
- Placeholder scan: no task uses placeholder language for required behavior.
- Type consistency: public API names are `PaxWriterOptions`, `PaxWriterCallbacks`, `PaxChunk`, `PaxWriteResult`, and `write_pax` throughout.
- Commit policy: this repository session should not create commits unless the user explicitly requests them.
