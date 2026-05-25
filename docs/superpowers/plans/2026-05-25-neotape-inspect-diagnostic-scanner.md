# neotape-inspect Diagnostic Scanner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redesign `neotape-inspect` as a best-effort diagnostic scanner with default first-header mode and `-r` / `--read` deep frame validation for both `spool:` and `tape:` sources.

**Architecture:** Keep the implementation in `src/neotape_inspect.cpp`, but reorganize it around a source-neutral sequence of physical tape files. `spool:` and `tape:` backends provide labels and record streams; shared inspect logic parses headers, validates consistency, prints line-oriented diagnostics, accumulates errors, and exits `1` when malformed data is found.

**Tech Stack:** C++20, `getopt_long`, NeoTape generated fixed-header parsers, BLAKE3, POSIX tape/file IO, shell smoke tests, GNU Make.

---

## File Structure

- Modify `src/neotape_inspect.cpp`: replace fail-fast validation with diagnostic scanner state, `-r/--read` parsing, source-neutral spool/tape scanning, line-oriented output, and final exit status.
- Create `tests/smoke_inspect_diagnostic.sh`: builds small spool archives, verifies default and `--read` success, corrupts headers and payloads, verifies continued scanning and exit `1`.
- Modify `Makefile`: add `tests/smoke_inspect_diagnostic.sh` to `make test` and add a convenience `test_inspect_diagnostic` target.
- Keep `docs/superpowers/specs/2026-05-25-neotape-inspect-diagnostic-design.md` as the approved design reference.

Do not introduce new public headers unless `src/neotape_inspect.cpp` becomes too large to reason about. Prefer local structs and functions inside the anonymous namespace, following the existing file style.

---

### Task 1: Add Failing Diagnostic Smoke Test

**Files:**
- Create: `tests/smoke_inspect_diagnostic.sh`
- Modify: `Makefile:44-113`

- [ ] **Step 1: Create the smoke test script**

Create `tests/smoke_inspect_diagnostic.sh` with this content:

```sh
#!/bin/sh
set -eu

root=/tmp/neotape-inspect-root
spool=/tmp/neotape-inspect.spool
bad_header_spool=/tmp/neotape-inspect-bad-header.spool
bad_payload_spool=/tmp/neotape-inspect-bad-payload.spool
default_out=/tmp/neotape-inspect-default.out
read_out=/tmp/neotape-inspect-read.out
bad_header_out=/tmp/neotape-inspect-bad-header.out
bad_payload_out=/tmp/neotape-inspect-bad-payload.out

rm -rf "$root" "$spool" "$bad_header_spool" "$bad_payload_spool" \
    "$default_out" "$read_out" "$bad_header_out" "$bad_payload_out"
mkdir -p "$root/src"
dd if=/dev/zero of="$root/src/large.bin" bs=1024 count=80 2>/dev/null
printf 'inspect diagnostic\n' > "$root/src/readme.txt"

bin/neotape init "spool:$spool" --label INSPECT --virtual-tape-size 64M >/dev/null
bin/neotape backup --target "spool:$spool" -C "$root" src \
    --name inspect-smoke >/dev/null

bin/neotape-inspect "spool:$spool" > "$default_out"
grep -q 'medium' "$default_out"
grep -q 'volume' "$default_out"
grep -q 'frame' "$default_out"
grep -q 'archive_end' "$default_out"
grep -q 'summary: .*errors=0' "$default_out"

bin/neotape-inspect --read "spool:$spool" > "$read_out"
grep -q 'frame index=0' "$read_out"
grep -q 'summary: .*errors=0' "$read_out"

cp -R "$spool" "$bad_header_spool"
printf 'BROKEN!!' | dd of="$bad_header_spool/tape-file-000002.slice-000001.nts" \
    bs=1 seek=0 conv=notrunc 2>/dev/null
if bin/neotape-inspect "spool:$bad_header_spool" > "$bad_header_out"; then
    printf 'expected malformed header inspect to exit 1\n' >&2
    exit 1
fi
grep -q 'malformed' "$bad_header_out"
grep -q 'bad magic' "$bad_header_out"
grep -q 'archive_end' "$bad_header_out"
grep -q 'summary: .*errors=' "$bad_header_out"

cp -R "$spool" "$bad_payload_spool"
printf 'X' | dd of="$bad_payload_spool/tape-file-000002.slice-000001.nts" \
    bs=1 seek=1024 conv=notrunc 2>/dev/null
if bin/neotape-inspect --read "spool:$bad_payload_spool" > "$bad_payload_out"; then
    printf 'expected malformed payload inspect to exit 1\n' >&2
    exit 1
fi
grep -q 'payload BLAKE3 mismatch' "$bad_payload_out"
grep -q 'summary: .*errors=' "$bad_payload_out"
```

- [ ] **Step 2: Make the script executable**

Run:

```sh
chmod +x tests/smoke_inspect_diagnostic.sh
```

Expected: no output.

- [ ] **Step 3: Wire the test into Makefile**

Modify `.PHONY` near `Makefile:44` from:

```make
.PHONY: all clean countline format test test_pax_cli test_tape_backup_wiring test_file_backed_tape generate tidy
```

to:

```make
.PHONY: all clean countline format test test_pax_cli test_tape_backup_wiring test_inspect_diagnostic test_file_backed_tape generate tidy
```

Modify the `test` target near `Makefile:101-107` to add the diagnostic smoke script after the pax smoke test:

```make
test: $(BINDIR)/test_tape $(BINDIR)/test_cli $(BINDIR)/test_restore_validation $(BINDIR)/neotape $(BINDIR)/neotape-inspect

	$(BINDIR)/test_tape
	$(BINDIR)/test_cli
	$(BINDIR)/test_restore_validation
	sh tests/smoke_restore_validation.sh
	sh tests/smoke_pax_backup_restore.sh
	sh tests/smoke_inspect_diagnostic.sh
	sh tests/smoke_tape_backup_wiring.sh
```

Add this target after `test_tape_backup_wiring`:

```make
test_inspect_diagnostic: $(BINDIR)/neotape $(BINDIR)/neotape-inspect
	sh tests/smoke_inspect_diagnostic.sh
```

- [ ] **Step 4: Run the new test and verify it fails for the right reason**

Run:

```sh
make bin/neotape bin/neotape-inspect && sh tests/smoke_inspect_diagnostic.sh
```

Expected before implementation: fails because `bin/neotape-inspect --read` is not accepted, or because fail-fast inspect does not produce the expected diagnostic output.

---

### Task 2: Add CLI Option and Diagnostic Result State

**Files:**
- Modify: `src/neotape_inspect.cpp:27-83`

- [ ] **Step 1: Extend options parsing**

Modify `Options`, `usage`, and `parse_args` so `-r` / `--read` is accepted:

```cpp
struct Options {
    neotape::Locator source;
    bool read_all = false;
};

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} [-r|--read] <spool-dir|spool:<dir>|tape:<device>>\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    Options opts;
    static const struct option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"read", no_argument, nullptr, 'r'},
        {nullptr, 0, nullptr, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "hr", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case 'r':
            opts.read_all = true;
            break;
        case '?':
            std::exit(2);
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        std::exit(2);
    }
    string source = argv[optind++];
    if (optind != argc) {
        usage(argv[0]);
        std::exit(2);
    }
    if (source.find(':') == string::npos) {
        opts.source = neotape::Locator{"spool", source};
        return opts;
    }

    opts.source = neotape::parse_locator(source);
    if (opts.source.kind != "spool" && opts.source.kind != "tape")
        fail("inspect source must be spool: or tape:");
    return opts;
}
```

- [ ] **Step 2: Add diagnostic counters**

Add this struct after `InspectState`:

```cpp
struct DiagnosticState {
    uint64_t files = 0;
    uint64_t frames = 0;
    uint64_t malformed = 0;
    uint64_t errors = 0;
    uint64_t warnings = 0;

    void error() {
        ++errors;
    }

    void malformed_error() {
        ++malformed;
        ++errors;
    }
};
```

- [ ] **Step 3: Build to verify parsing compiles**

Run:

```sh
make bin/neotape-inspect
```

Expected: build succeeds. The smoke test still fails because scanner behavior is not implemented yet.

---

### Task 3: Replace Fail-Fast Checks With Diagnostic Reporting Helpers

**Files:**
- Modify: `src/neotape_inspect.cpp:114-263`

- [ ] **Step 1: Add non-throwing error helpers**

Add these helpers after `require` or replace `require` call sites progressively:

```cpp
bool report_error(DiagnosticState &diag, const string &label,
                  const string &message) {
    diag.error();
    std::cout << format("{}: error reason=\"{}\"\n", label, message);
    return false;
}

bool report_malformed(DiagnosticState &diag, const string &label,
                      const string &message, size_t bytes) {
    diag.malformed_error();
    std::cout << format("{}: malformed reason=\"{}\" bytes={}\n", label,
                        message, bytes);
    return false;
}
```

- [ ] **Step 2: Add parse wrapper that catches malformed headers**

Add this helper near the record validation section:

```cpp
std::optional<neotape::ParsedHeader>
try_parse_header(const string &label, const vector<uint8_t> &record,
                 DiagnosticState &diag) {
    if (record.size() < neotape::fixed_header_size) {
        report_malformed(diag, label, "short fixed header", record.size());
        return std::nullopt;
    }
    try {
        return neotape::parse_fixed_header(record.data(), record.size());
    } catch (const std::exception &e) {
        report_malformed(diag, label, e.what(), record.size());
        return std::nullopt;
    }
}
```

- [ ] **Step 3: Convert header inspection functions to return bool**

Change `inspect_medium`, `inspect_volume`, `inspect_frame`, and
`inspect_archive_end` to return `bool` and accept `DiagnosticState &diag`.
For each old `require(condition, message)`, replace with:

```cpp
if (!condition)
    return report_error(diag, label_or_path_string, "specific reason");
```

Keep the existing decoded output lines, but add key fields from the spec:

```cpp
std::cout << format(
    "{}: volume version=1 crc=ok archive={} name={} volume={} block={} profile={} written={} flags=0x{:04x}\n",
    label, header.archive_uuid, header.archive_name, header.volume_seq_num,
    header.volume_block_size, neotape::payload_profile_name(header.payload_profile),
    header.volume_write_at_utc, header.flags);
```

Use equivalent field-rich lines for `medium`, `frame`, and `archive_end`.

- [ ] **Step 4: Build after helper conversion**

Run:

```sh
make bin/neotape-inspect
```

Expected: build succeeds. Some traversal code may still be fail-fast until later tasks; do not run the smoke test yet if old callers still require strict behavior.

---

### Task 4: Implement Source-Neutral Spool Scanner

**Files:**
- Modify: `src/neotape_inspect.cpp:86-319`

- [ ] **Step 1: Update spool traversal for flat `.nts` files**

Replace the old nested directory-only traversal in `inspect_spool` with one ordered list that supports the current flat layout and old directory layout:

```cpp
vector<fs::path> spool_tape_files(const fs::path &root) {
    vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".nts")
            files.push_back(entry.path());
    }
    if (!files.empty()) {
        std::ranges::sort(files);
        return files;
    }

    for (const fs::path &volume_dir : sorted_dirs(root)) {
        for (const fs::path &file : sorted_files(volume_dir))
            files.push_back(file);
    }
    std::ranges::sort(files);
    return files;
}
```

- [ ] **Step 2: Add label helper**

Add:

```cpp
string spool_label(const fs::path &root, const fs::path &file) {
    fs::path rel = fs::relative(file, root);
    return format("spool:{}", rel.string());
}
```

- [ ] **Step 3: Add first-record spool inspection**

Implement:

```cpp
void inspect_first_record(const string &label, InspectState &state,
                          DiagnosticState &diag,
                          const vector<uint8_t> &record) {
    auto parsed = try_parse_header(label, record, diag);
    if (!parsed)
        return;
    if (parsed->medium) {
        inspect_medium(label, diag, *parsed->medium, record.size());
    } else if (parsed->volume) {
        inspect_volume(label, state, diag, *parsed->volume, record.size());
    } else if (parsed->frame) {
        inspect_frame(label, state, diag, *parsed->frame, record, std::nullopt,
                      std::nullopt);
    } else if (parsed->archive_end) {
        inspect_archive_end(label, state, diag, *parsed->archive_end,
                            record.size());
    } else {
        report_malformed(diag, label,
                         format("unknown header type {}",
                                neotape::header_type_name(parsed->type)),
                         record.size());
    }
}
```

If the actual `inspect_frame` signature from Task 3 does not yet include index
and offset, add optional parameters now so default mode can omit them and deep
mode can print them.

- [ ] **Step 4: Implement default-mode spool scan**

Update `inspect_spool` so when `opts.read_all == false`, it reads each spool
tape file, increments `diag.files`, inspects only the first record-sized chunk,
and keeps scanning after malformed files:

```cpp
void inspect_spool(const Options &opts, InspectState &state,
                   DiagnosticState &diag) {
    fs::path spool_dir(opts.source.locator);
    if (!fs::is_directory(spool_dir))
        fail(format("{} is not a directory", spool_dir.string()));

    for (const fs::path &file : spool_tape_files(spool_dir)) {
        ++diag.files;
        vector<uint8_t> bytes = read_file(file);
        string label = spool_label(spool_dir, file);
        if (opts.read_all)
            inspect_spool_file_deep(label, state, diag, bytes);
        else
            inspect_first_record(label, state, diag, bytes);
    }
}
```

`inspect_spool_file_deep` is added in Task 5; temporarily make it call
`inspect_first_record` so this task compiles.

- [ ] **Step 5: Build and run default part of smoke manually**

Run:

```sh
make bin/neotape bin/neotape-inspect && bin/neotape-inspect spool:/tmp/neotape-inspect.spool >/tmp/neotape-inspect-default.out
```

Expected after creating the test spool in Task 1 or by running the smoke script until its first failure: default output prints medium, volume, frame, archive_end lines and does not abort on valid input.

---

### Task 5: Implement Deep Frame Reading for Spool

**Files:**
- Modify: `src/neotape_inspect.cpp`

- [ ] **Step 1: Add frame validation helper with index and offset**

Update `inspect_frame` so it can print optional `index` and `offset` fields:

```cpp
string frame_position_fields(std::optional<uint64_t> index,
                             std::optional<uint64_t> offset) {
    string fields;
    if (index)
        fields += format(" index={}", *index);
    if (offset)
        fields += format(" offset={}", *offset);
    return fields;
}
```

Use it in the frame line:

```cpp
std::cout << format(
    "{}: frame{} version=1 crc=ok archive={} volume={} global={} slice={} within={} payload={} type={} flags=0x{:04x}\n",
    label, frame_position_fields(index, offset), header.archive_uuid,
    header.volume_seq_num, header.global_frame_seq_num,
    header.logical_slice_seq_num, header.frame_seq_num_within_slice,
    header.frame_payload_size,
    neotape::frame_content_type_name(header.frame_content_type),
    header.flags);
```

- [ ] **Step 2: Add record validator for deep reads**

Implement:

```cpp
void inspect_deep_record(const string &label, InspectState &state,
                         DiagnosticState &diag, const vector<uint8_t> &record,
                         uint64_t index, uint64_t offset) {
    auto parsed = try_parse_header(label, record, diag);
    if (!parsed)
        return;
    if (!parsed->frame) {
        report_malformed(diag, label, "non-frame record inside frame file",
                         record.size());
        return;
    }
    if (inspect_frame(label, state, diag, *parsed->frame, record, index, offset))
        ++diag.frames;
}
```

- [ ] **Step 3: Implement `inspect_spool_file_deep`**

Implement:

```cpp
void inspect_spool_file_deep(const string &label, InspectState &state,
                             DiagnosticState &diag,
                             const vector<uint8_t> &bytes) {
    auto parsed = try_parse_header(label, bytes, diag);
    if (!parsed)
        return;
    if (!parsed->frame) {
        inspect_first_record(label, state, diag, bytes);
        return;
    }
    if (state.volume_block_size == 0) {
        report_error(diag, label, "frame appears before volume header");
        return;
    }
    if (bytes.size() % state.volume_block_size != 0) {
        report_error(diag, label,
                     "frame file size is not a multiple of block size");
    }

    uint64_t index = 0;
    for (size_t offset = 0; offset + state.volume_block_size <= bytes.size();
         offset += state.volume_block_size, ++index) {
        vector<uint8_t> record(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + state.volume_block_size));
        inspect_deep_record(label, state, diag, record, index, offset);
    }
}
```

- [ ] **Step 4: Run the inspect diagnostic smoke test**

Run:

```sh
make bin/neotape bin/neotape-inspect && sh tests/smoke_inspect_diagnostic.sh
```

Expected: may still fail on exact output or missing final summary, but it should reach `--read` and report payload hash mismatches rather than rejecting the option.

---

### Task 6: Implement Tape Scanner With Best-Effort Continuation

**Files:**
- Modify: `src/neotape_inspect.cpp:321-400`

- [ ] **Step 1: Update tape read helper to return optional records**

Replace fail-fast tape reads with:

```cpp
std::optional<vector<uint8_t>> read_tape_record(mt::TapeDevice &dev,
                                                size_t size,
                                                const string &label,
                                                DiagnosticState &diag,
                                                bool allow_eof = false) {
    vector<uint8_t> record(size);
    ssize_t n = ::read(dev.fd(), record.data(), record.size());
    if (n < 0) {
        report_error(diag, label, format("read: {}", std::strerror(errno)));
        return std::nullopt;
    }
    if (n == 0 && allow_eof)
        return vector<uint8_t>{};
    if (n == 0) {
        report_error(diag, label, "short read from tape");
        return std::nullopt;
    }
    record.resize(static_cast<size_t>(n));
    return record;
}
```

- [ ] **Step 2: Update filemark advance helper**

Replace `advance_tape_file` with:

```cpp
bool advance_tape_file(mt::TapeDevice &dev, DiagnosticState &diag,
                       const string &label) {
    try {
        dev.space_fwd_filemark();
        return true;
    } catch (const mt::Error &e) {
        report_error(diag, label, e.what());
        return false;
    }
}
```

- [ ] **Step 3: Implement default tape scan**

Rewrite `inspect_tape` to loop over tape files, inspect first records, and stop
only on EOF or unadvanceable tape errors:

```cpp
void inspect_tape(const Options &opts, InspectState &state,
                  DiagnosticState &diag) {
    mt::TapeDevice dev(opts.source.locator, false);
    uint64_t file_num = 0;
    size_t read_size = 8 * 1024 * 1024;

    while (true) {
        string label = format("tape:file-{}", file_num);
        auto record = read_tape_record(dev, read_size, label, diag, file_num != 0);
        if (!record)
            break;
        if (record->empty())
            break;
        ++diag.files;

        if (opts.read_all)
            inspect_tape_file_deep(dev, label, state, diag, *record);
        else
            inspect_first_record(label, state, diag, *record);

        if (!advance_tape_file(dev, diag, label))
            break;
        ++file_num;
        if (state.volume_block_size != 0)
            read_size = state.volume_block_size;
    }
}
```

`inspect_tape_file_deep` is added in the next step; temporarily make it inspect
the first record only so this compiles.

- [ ] **Step 4: Implement deep tape file scan**

For tape frame files, read the first record already supplied, then continue
reading `state.volume_block_size` records until filemark (`read()` returns `0`) or
an error. Do not call `space_fwd_filemark` from inside this helper; the caller
handles filemark advance between tape files.

```cpp
void inspect_tape_file_deep(mt::TapeDevice &dev, const string &label,
                            InspectState &state, DiagnosticState &diag,
                            const vector<uint8_t> &first_record) {
    auto parsed = try_parse_header(label, first_record, diag);
    if (!parsed)
        return;
    if (!parsed->frame) {
        inspect_first_record(label, state, diag, first_record);
        return;
    }
    if (state.volume_block_size == 0) {
        report_error(diag, label, "frame appears before volume header");
        return;
    }

    inspect_deep_record(label, state, diag, first_record, 0, 0);
    uint64_t index = 1;
    uint64_t offset = state.volume_block_size;
    while (true) {
        auto record = read_tape_record(dev, state.volume_block_size, label, diag,
                                       true);
        if (!record)
            return;
        if (record->empty())
            return;
        inspect_deep_record(label, state, diag, *record, index, offset);
        ++index;
        offset += state.volume_block_size;
    }
}
```

- [ ] **Step 5: Build `neotape-inspect`**

Run:

```sh
make bin/neotape-inspect
```

Expected: build succeeds. Do not require real tape hardware for this task.

---

### Task 7: Add Summary and Final Exit Behavior

**Files:**
- Modify: `src/neotape_inspect.cpp:360-415`

- [ ] **Step 1: Add summary printer**

Add:

```cpp
void print_summary(const InspectState &state, const DiagnosticState &diag) {
    std::cout << format(
        "summary: files={} malformed={} errors={} warnings={} archive={} volumes={} frames={} slices={} end={}\n",
        diag.files, diag.malformed, diag.errors, diag.warnings,
        state.archive_uuid.empty() ? string("-") : state.archive_uuid,
        state.expected_volume_seq_num > 0 ? state.expected_volume_seq_num - 1 : 0,
        state.expected_global_frame_seq_num > 0 ? state.expected_global_frame_seq_num - 1 : 0,
        state.expected_slice_seq_num > 0 ? state.expected_slice_seq_num - 1 : 0,
        state.saw_archive_end ? "yes" : "no");
}
```

- [ ] **Step 2: Report missing archive end without aborting**

After source scanning finishes, if an archive started but no archive end was
seen, report an error:

```cpp
if (!state.archive_uuid.empty() && !state.saw_archive_end)
    report_error(diag, "summary", "missing Archive End Header");
```

- [ ] **Step 3: Return final status from `main`**

Replace main body with:

```cpp
int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        InspectState state;
        DiagnosticState diag;
        if (opts.source.kind == "tape")
            inspect_tape(opts, state, diag);
        else
            inspect_spool(opts, state, diag);
        if (!state.archive_uuid.empty() && !state.saw_archive_end)
            report_error(diag, "summary", "missing Archive End Header");
        print_summary(state, diag);
        return diag.errors == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
```

- [ ] **Step 4: Run diagnostic smoke test**

Run:

```sh
make bin/neotape bin/neotape-inspect && sh tests/smoke_inspect_diagnostic.sh
```

Expected: passes.

---

### Task 8: Full Verification and Formatting

**Files:**
- Modify if needed: `src/neotape_inspect.cpp`, `tests/smoke_inspect_diagnostic.sh`, `Makefile`

- [ ] **Step 1: Format C++ changes**

Run:

```sh
make format
```

Expected: command succeeds. Review generated diff to ensure no generated files were hand-edited.

- [ ] **Step 2: Build everything**

Run:

```sh
make -j "$(nproc)"
```

Expected: all binaries build, including `bin/neotape-inspect`.

- [ ] **Step 3: Run all tests**

Run:

```sh
make test
```

Expected: `bin/test_tape`, `bin/test_cli`, `bin/test_restore_validation`, and all smoke scripts pass.

- [ ] **Step 4: Review final diff**

Run:

```sh
git diff -- src/neotape_inspect.cpp tests/smoke_inspect_diagnostic.sh Makefile docs/superpowers/specs/2026-05-25-neotape-inspect-diagnostic-design.md docs/superpowers/plans/2026-05-25-neotape-inspect-diagnostic-scanner.md
```

Expected: diff is limited to the diagnostic inspect implementation, test wiring, the approved spec, and this plan.

---

## Self-Review

- Spec coverage: default first-header scan is covered by Tasks 4 and 6; `-r/--read` deep scan is covered by Tasks 5 and 6; equal `spool:`/`tape:` semantics are covered by the source-neutral helpers and separate backends; malformed-is-nonfatal with exit `1` is covered by Tasks 3 and 7; tests are covered by Tasks 1 and 8.
- Placeholder scan: no `TBD`, `TODO`, or unspecified implementation steps remain. Each code-changing step includes concrete code or exact replacement guidance.
- Type consistency: `Options::read_all`, `InspectState`, `DiagnosticState`, `inspect_spool`, `inspect_tape`, `inspect_first_record`, `inspect_spool_file_deep`, and `inspect_tape_file_deep` names are used consistently across tasks.
- Scope check: this plan is focused on `neotape-inspect` diagnostics only. It does not add JSON output, salvage behavior, or real-tape-dependent automated tests.
