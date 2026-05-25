# NeoTape Backup Pax Options Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `neotape backup` expose the same pax throughput/buffer controls and progress display behavior as `mt-pax`, using `-T` for io thread count to avoid colliding with existing `-t/--target`.

**Architecture:** Reuse existing `neotape::PaxWriterOptions` fields and the existing stats thread in `src/neotape_pax_writer.cpp`. Add only CLI parsing and plumbing in `src/neotape_write.cpp`; do not move the `"\r..."` progress format string because its leading carriage return is intentional.

**Tech Stack:** C++20, `getopt_long`, existing `neotape::parse_size`, existing Makefile tests and shell smoke tests.

---

## File Structure

- Modify `src/neotape_write.cpp`: extend `BackupOptions`, usage text, `getopt_long` tables, parsing, validation, and pax option plumbing for both spool and tape backup.
- Modify `tests/smoke_pax_backup_restore.sh`: add a CLI smoke invocation using `-P`, `-B`, and `-T` on a small spool backup so unsupported or misparsed options fail in `make test`.
- Do not modify `src/neotape_pax_writer.cpp` for progress formatting; it already prints `in/out/files/total/buffer` from the stats thread with the leading `"\r"`.

## Task 1: Add Backup CLI Options and Parsing

**Files:**
- Modify: `src/neotape_write.cpp:61-69`
- Modify: `src/neotape_write.cpp:281-351`

- [ ] **Step 1: Write the failing smoke test command**

Add a second planned backup command to `tests/smoke_pax_backup_restore.sh` after the existing planned backup succeeds:

```sh
rm -rf "$planned_spool"
bin/neotape init "spool:$planned_spool" --label PAXPLAN --virtual-tape-size 64M >/dev/null
bin/neotape backup --target "spool:$planned_spool" -p "$plan" \
    --name pax-planned-options -P 25 -B 8M -T 2 >/dev/null
bin/neotape restore --source "spool:$planned_spool" --output "$planned_archive" >/dev/null
test -s "$planned_archive"
```

Keep the existing planned backup coverage or replace only the command block if duplication becomes noisy. The important part is that `-P`, `-B`, and `-T` are accepted by `neotape backup`.

- [ ] **Step 2: Run the smoke test and verify it fails before implementation**

Run:

```sh
make bin/neotape && sh tests/smoke_pax_backup_restore.sh
```

Expected before implementation: failure from `getopt_long`/usage because `neotape backup` does not accept `-P`, `-B`, or `-T` yet.

- [ ] **Step 3: Extend `BackupOptions`**

In `src/neotape_write.cpp`, update `BackupOptions` to include pax writer controls:

```cpp
struct BackupOptions {
    neotape::Locator target;
    string archive_name = "pax";
    uint32_t volume_block_size = 4 * 1024 * 1024;
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
    std::optional<string> chdir_dir;
    std::optional<fs::path> plan_path;
    vector<fs::path> sources;
    size_t output_buf_size = 64UL * 1024 * 1024;
    unsigned buffer_percent = 0;
    unsigned io_thread = 1;
};
```

- [ ] **Step 4: Update backup usage text**

Change `backup_usage()` to mention the new options:

```cpp
void backup_usage() {
    std::cerr << "usage: neotape backup --target <locator> [-C <dir>] <path> "
                 "[path ...]\n"
                 "       neotape backup --target <locator> -p <plan>\n"
                 "       [--name <name>] [--volume-block-size <bytes>] "
                 "[--control=auto|none]\n"
                 "       [-P <buffer-percent>] [-B <bytes>] [-T <N>] "
                 "[--output-buffer-size <bytes>] [--io-thread <N>]\n";
}
```

- [ ] **Step 5: Add getopt entries and optstring**

In `parse_backup_args()`, update `long_opts` and the optstring:

```cpp
static const struct option long_opts[] = {
    {"target", required_argument, nullptr, 't'},
    {"name", required_argument, nullptr, 'n'},
    {"volume-block-size", required_argument, nullptr, 'b'},
    {"control", required_argument, nullptr, 'c'},
    {"plan", required_argument, nullptr, 'p'},
    {"directory", required_argument, nullptr, 'C'},
    {"buffer-percent", required_argument, nullptr, 'P'},
    {"output-buffer-size", required_argument, nullptr, 'B'},
    {"io-thread", required_argument, nullptr, 'T'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0}};
```

Use this optstring so short `-t` stays target, matching current `neotape backup`, and `-T` means io thread count:

```cpp
while ((c = getopt_long(argc, argv, "C:p:P:B:t:T:", long_opts, nullptr)) != -1) {
```

Do not change existing `-t/--target` behavior.

- [ ] **Step 6: Parse and validate new values**

Add switch cases:

```cpp
case 'P': {
    char *end = nullptr;
    unsigned long n = std::strtoul(optarg, &end, 10);
    if (end == optarg || *end != '\0' || n > 100)
        fail("-P requires a percent from 0 to 100");
    opts.buffer_percent = static_cast<unsigned>(n);
    break;
}
case 'B':
    opts.output_buf_size = static_cast<size_t>(
        neotape::parse_size(optarg, "output buffer size"));
    break;
case 'T': {
    char *end = nullptr;
    unsigned long n = std::strtoul(optarg, &end, 10);
    if (end == optarg || *end != '\0')
        fail("--io-thread requires a number");
    opts.io_thread = static_cast<unsigned>(n);
    break;
}
```

Do not add a separate `--buffer-percent` validation path; using `val = 'P'` makes it share the short option case.

## Task 2: Plumb Options Into Pax Writer

**Files:**
- Modify: `src/neotape_write.cpp:797-824`
- Modify: `src/neotape_write.cpp:841-868`

- [ ] **Step 1: Pass options in spool backup**

In `run_spool_pax_backup()`, after setting `pax.chdir_dir`, add:

```cpp
pax.output_buf_size = backup.output_buf_size;
pax.buffer_percent = backup.buffer_percent;
pax.io_thread = backup.io_thread;
```

- [ ] **Step 2: Pass options in tape backup**

In `run_tape_pax_backup()`, after setting `pax.chdir_dir`, add:

```cpp
pax.output_buf_size = backup.output_buf_size;
pax.buffer_percent = backup.buffer_percent;
pax.io_thread = backup.io_thread;
```

- [ ] **Step 3: Preserve progress output formatting**

Verify no changes were made to this existing code in `src/neotape_pax_writer.cpp`:

```cpp
cerr << format(
    "\rin @ {:>6}/s, out @ {:>6}/s, files @ {:>6}/s, "
    "{:>6} total, buffer {:3}% full  ",
```

The leading `"\r"` must remain at the beginning of the format string.

## Task 3: Verify and Report

**Files:**
- Verify: `src/neotape_write.cpp`
- Verify: `tests/smoke_pax_backup_restore.sh`

- [ ] **Step 1: Run focused smoke test**

Run:

```sh
make bin/neotape && sh tests/smoke_pax_backup_restore.sh
```

Expected: command exits 0; planned backup with `-P 25 -B 8M -T 2` succeeds.

- [ ] **Step 2: Run full verification**

Run:

```sh
make -j "$(nproc)" && make test
```

Expected: build exits 0 and all tests pass. `bin/test_tape` should report `0 failure(s)`.

- [ ] **Step 3: Inspect diff**

Run:

```sh
git diff -- src/neotape_write.cpp tests/smoke_pax_backup_restore.sh
```

Expected: diff only contains backup CLI parsing/plumbing and the smoke test update. Existing unrelated dirty submodules or docs remain untouched.

---

## Self-Review

- Spec coverage: plan covers progress display parity by reusing existing pax stats output, buffer size via `-B/--output-buffer-size`, buffer waterline via `-P/--buffer-percent`, and thread count via `-T/--io-thread`.
- Ambiguity resolved: `neotape backup` keeps existing `-t/--target`; io thread count uses `-T/--io-thread`.
- Placeholder scan: no TBD/TODO placeholders remain.
