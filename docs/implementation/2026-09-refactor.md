# CLI and streaming refactor

Status: implemented and verified. This tracks the September 2026 implementation review.

## Constraints

- Preserve on-media and socket formats, byte-opaque pathnames, signing and FEC.
- Preserve archiver's mbuffer-style progress and writer exit codes 0/1/2/3.
- Keep bounded asynchronous output and ACK-based retention/back-pressure.
- Keep TapeDevice forward/backward tape-file positioning available for future
  slice checkpoints. RecordSink narrows streaming writes only; it does not
  remove media control. Checkpoint seek must quiesce I/O and reseed stream state.
- Keep scan's first-record-only traversal and dump's validation-free copying.
- Share mechanisms; keep validation and reporting policy with each tool.
- Include the three preceding reliability fixes and retain their coverage.

## Work checklist

- [x] Fix EOT receiver wakeup, unacknowledged archive-end completion, and planned
  progress/pipeline lifetime race (baseline: 25 tests; planned turnover under TSan).
- [x] Unify writer record/filemark/archive-end output, ACK ownership, and terminal
  results in a single-volume session; separate CLI/media setup.
- [x] Replace null/capacity TapeDevice subclasses with a narrow record sink.
- [x] Share media locators, spool naming/enumeration, and record/filemark/EOD
  reading between read, inspect, scan, and dump where their semantics match.
- [x] Share periodic progress lifecycle/rates; preserve tool-specific rendering.
- [x] Share pax archive-session setup/cleanup between planned and walked input.
- [x] Introduce a streaming plan reader/writer codec used by planner and pax.
- [x] Consolidate small test fixtures and remove assertions tied to incidental
  help/debug/report formatting; retain format and observable-behavior coverage.
- [x] Build and run relevant/full tests; check race-sensitive changes with TSan.
- [x] Record final boundaries, verification, and remaining limitations below.

## Verification and implementation notes

The baseline reliability fixes are included in this refactor. Progress checkboxes represent completed
implementation and verification, not merely files moved to a different directory.

## Final module boundaries

| Component | Owns | Deliberately leaves to callers |
| --- | --- | --- |
| `writer.hpp` / `write_volume` | Validation, bounded queue, ordered record/filemark/archive-end writes, ACK, terminal result | Authentication before opening media, rewind/append, bundle installation, CLI exit mapping |
| `RecordSink` | Streaming writes and capacity accounting; null discards through the same session | Tape positioning and ioctl controls remain on the caller-owned `TapeDevice` |
| `media.hpp` | Media locators, spool ordering, record/filemark/EOD events, tape-file skip | Archive validation, salvage policy, display; scan still reads only the first header per spool file |
| `progress.hpp` | Timer lifetime, rates, count formatting and buffer ratio | Tool-specific mbuffer renderers and final diagnostics |
| `plan.hpp` | NUL-LF framing and plan metadata codec, one-record-at-a-time input | Traversal, slice packing, filesystem metadata collection |
| Pax archive session | Common resource cleanup, link resolution, slice pipeline and end marker | Planned input and directory traversal retain their distinct entry sources |

The writer output thread is the sole ACK owner. A successfully written final
record follows the same path as content; EOT/error wakes both bounded-queue
waiters and a blocked socket receiver. Successful write is not a new durability
guarantee: tape/spool persistence semantics remain those of the existing backend.
Progress snapshots in the writer are taken under one state lock.

Positioning remains available through `TapeDevice::space_fwd`,
`space_bwd`, `space_fwd_filemark`, `space_bwd_filemark`, rewind and seek.
A future checkpoint coordinator must drain or cancel the session before
positioning, then reconstruct capacity/sequence/validation state. This refactor
does not define a checkpoint format or resume protocol.

## Test scope and limits

- Retention-window-one null writes and real spool round-trip across volumes.
- Unacknowledged archive end is replayed after a protocol error.
- Slice turnover while progress samples, with payload verification under TSan.
- Spool boundaries, truncated records and skipping an unreadable tail.
- Planner-to-pax opaque pathname/symlink round-trip, including embedded LF.
- Restore tests compare directly with source files, not another run of the
  same encoder. Shared fixture helpers replace repeated process checks and
  file-pattern generation.
- No physical tape hardware was exercised. EIO/filemark and positioning
  behavior still needs real-device regression before checkpoint work.
- This is not a wholesale rewrite of all old unit tests; legacy monolithic
  Catch2 cases can be split when their corresponding modules next change.

## Completed verification

- `cmake --build --preset dev`: passed.
- `ctest --preset dev`: 28/28 passed (28.13 s).
- ThreadSanitizer: planned slice turnover and capacity-limited null writer
  integration cases passed with `TSAN_OPTIONS=halt_on_error=1`.
  Sanitizer build artifacts are isolated in `build/review-tsan`.
- `git diff --check`: passed.
- Production C++/headers: 11,609 lines at `1e57a3b` → 10,762 lines
  including all new shared modules (net -847, about 7.3%). Counts include
  comments/blank lines, exclude third-party/generated/build files and CMake.
  The comparison also includes the three preceding reliability fixes.
- Tests/support/benchmark/CMake: 3,732 → 3,839 lines. Added behavioral
  regression coverage accounts for the net increase despite fixture and
  formatting-assertion cleanup.
