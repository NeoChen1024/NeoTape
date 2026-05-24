# Tape Archive Sink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the PAX streaming backup path reusable for tape targets without requiring real LTO hardware for regression tests.

**Architecture:** Extract the frame-writing state machine behind a small record writer interface. Keep `SpoolArchiveSink` behavior intact, add a file-backed test sink that exercises the same record-level tape framing, then wire `backup --target tape:` to the existing `mt::write_tape_archive()`/tape writer path only after tests prove the shared stream-to-record behavior.

**Tech Stack:** C++20, GNU Make, existing NeoTape format helpers, existing `TapeDevice`/`TapeWriter`, shell smoke tests.

---

## Scope Check

This plan covers one subsystem: a testable archive sink boundary for streaming PAX writes. It does not implement full interactive hardware volume-change behavior or tape restore scanning; those remain separate steps.

## File Structure

- Modify `include/neotape/tape_writer.hpp`: add a callback-friendly tape write entry point after the sink is validated.
- Modify `src/neotape_tape_writer.cpp`: keep the current tape writer state machine, add a callback-based payload writer so callers can stream PAX chunks without a temp file.
- Modify `src/neotape_write.cpp`: route `backup --target tape:` into the callback-based tape writer.
- Create `tests/smoke_tape_pax_file_sink.sh`: hardware-free smoke using a file-backed or spool-backed fixture that proves the callback path emits parseable NeoTape records.
- Modify `Makefile`: add the smoke to a targeted test rule first, then to `make test` when stable.

## Task 1: Hardware-Free Tape Callback Smoke

**Files:**
- Create: `tests/smoke_tape_pax_file_sink.sh`
- Modify: `Makefile`
- Modify: `src/neotape_write.cpp`

- [ ] **Step 1: Write failing smoke test**

Create `tests/smoke_tape_pax_file_sink.sh`:

```sh
#!/bin/sh
set -eu

root=/tmp/neotape-tape-sink-root
spool=/tmp/neotape-tape-sink.spool
archive=/tmp/neotape-tape-sink.tar
out=/tmp/neotape-tape-sink-out

rm -rf "$root" "$spool" "$archive" "$out"
mkdir -p "$root/src" "$out"
printf 'tape sink pax\n' > "$root/src/file.txt"

bin/neotape init "spool:$spool" --label TAPESINK --virtual-tape-size 64M >/dev/null
bin/neotape backup --target "spool:$spool" -C "$root" src --name tape-sink >/dev/null
bin/neotape restore --source "spool:$spool" --output "$archive" >/dev/null
bsdtar -xpf "$archive" -C "$out"
cmp "$root/src/file.txt" "$out/src/file.txt"
```

This initially reuses spool because no file-backed tape writer exists yet; it defines the behavior the tape callback writer must continue to match.

- [ ] **Step 2: Add targeted Makefile rule**

Add:

```make
test_tape_sink: $(BINDIR)/neotape
	sh tests/smoke_tape_pax_file_sink.sh
```

- [ ] **Step 3: Run test**

Run: `make test_tape_sink`

Expected: PASS before refactor. This is a characterization guard, not the RED test.

## Task 2: Callback-Based Tape Writer Entry

**Files:**
- Modify: `include/neotape/tape_writer.hpp`
- Modify: `src/neotape_tape_writer.cpp`
- Modify: `src/neotape_write.cpp`

- [ ] **Step 1: Add failing CLI test for tape backup wiring**

Run:

```sh
bin/neotape backup --target tape:/tmp/not-a-tape src 2>/tmp/neotape-tape-backup.err || true
grep -q 'backup currently supports spool' /tmp/neotape-tape-backup.err
```

Expected before implementation: grep succeeds, proving tape backup is still blocked at CLI level.

- [ ] **Step 2: Add callback API**

In `include/neotape/tape_writer.hpp`, add:

```cpp
#include <cstddef>
#include <functional>

struct TapePayloadCallbacks {
    std::function<void(std::function<void(const uint8_t *, std::size_t)>)> produce;
};

void write_tape_archive_from_chunks(const TapeWriterOptions &opts,
                                    TapePayloadCallbacks callbacks);
```

- [ ] **Step 3: Implement minimal callback API**

In `src/neotape_tape_writer.cpp`, add an overload that initializes the same `WriterState`, writes the Volume Header, calls `callbacks.produce()` with a lambda that writes non-ending content frames, then writes one empty ending frame and the Archive End Header.

- [ ] **Step 4: Wire `backup --target tape:`**

In `src/neotape_write.cpp`, replace the current tape-target error with a call that runs `neotape::write_pax()` and forwards each `PaxChunk` to `write_tape_archive_from_chunks()`.

- [ ] **Step 5: Verify build**

Run: `make bin/neotape`

Expected: build succeeds. Hardware execution is not required in this task.

## Task 3: Final Verification

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Run full verification**

Run: `make -j "$(nproc)" && make test && make test_tape_sink`

Expected: all commands exit 0.

- [ ] **Step 2: Commit**

Run:

```sh
git add include/neotape/tape_writer.hpp src/neotape_tape_writer.cpp src/neotape_write.cpp Makefile tests/smoke_tape_pax_file_sink.sh
git commit -m "feat(cli): prepare streaming tape backup sink"
```

## Self-Review

- Covers the approved option 1: testable sink path before real LTO smoke.
- No temp PAX file is introduced.
- Tape restore and full interactive volume-change remain intentionally outside this plan.
