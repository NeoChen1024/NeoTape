# Project review revalidation

Date: 2026-06-18

Scope: re-check the fixes attempted after
`docs/archive/2026-06-17-project-review-findings.md`.

Result: the two findings that remained open in the first revalidation pass
(`R1`, `R2`) are now fixed in the current tree.

## Confirmed fixes

### C1. F2 restore-mode metadata handling now distinguishes advisory hash failures from fatal structural failures

Status: fixed in implementation

Severity: high

Evidence:

- `FrameValidator::validate_restore_frame()` now centralizes restore-mode
  policy in `include/neotape/validate.hpp` and
  `src/neotape_validate.cpp`.
- Only `ch_metadata` `frame_hash` mismatches are downgraded to warnings; any
  structural validator failure remains fatal in
  `src/neotape_validate.cpp`.
- `neotape-extractor` now consumes that policy directly instead of warning on
  every metadata validator error in `src/neotape_extractor.cpp`.

Regression coverage:

- `tests/test_validate.cpp` now covers:
  - metadata hash mismatch is warning-only and still advances restore state
  - metadata `archive_uuid` mismatch remains fatal
- `sh tests/smoke_tcp_extract.sh` still passes end-to-end on the extractor
  path after the policy change.

### C2. F4 validator now enforces `START`/`END` boundaries at archive start, slice boundaries, and archive termination

Status: fixed in implementation

Severity: medium

Evidence:

- `FrameValidator` now rejects `archive_end` when the preceding data/metadata
  group did not terminate with `END` in `src/neotape_validate.cpp`.
- `FrameValidator` now treats the first frame, the first frame of a new slice,
  and the first frame after a channel transition as mandatory `START` points in
  `src/neotape_validate.cpp`.
- The validator also rejects multiple same-channel groups within one logical
  slice, matching the current spec's "at most one contiguous group per channel"
  rule.

Regression coverage:

- `tests/test_validate.cpp` now covers:
  - first frame missing `START`
  - new-slice same-channel frame missing `START`
  - `archive_end` after a non-`END` content frame
  - multiple same-channel groups in one slice
- `sh tests/smoke_inspect.sh` still passes after the stricter validator
  changes.

### C3. F1 archiver content frames now use shared channel-group sequencing

Status: fixed in implementation

Evidence:

- `ContentFrameBuilder` now owns `frame_seq_num_within_channel`, `START` on the
  first frame, and `END` on `flush()` in
  `include/neotape/frame_builder.hpp:72-129` and
  `src/neotape_frame_builder.cpp:61-132`.
- `neotape-archiver` now builds content records through that shared builder in
  `src/neotape_tcp_server.cpp:58-91`.

Residual risk:

- `tests/smoke_tcp_archive_multi.sh:54-90` still does not assert
  `frame_seq_num_within_channel` continuity or exact `START`/`END` placement.

### C4. F3 `spool:` targets now honor `--erase` through write-mode rewind semantics

Status: fixed for the active CLI path

Evidence:

- `neotape-write` still maps `--erase` to `rewind()` in
  `src/neotape_write_cmd.cpp:548-552`.
- `SpoolTapeDevice::do_mtop(MTREW)` now removes existing finalized `.nts`
  files, resets numbering/state, and opens a fresh pending file in
  `src/neotape_tape.cpp:598-632`.

Residual risk:

- There is still no dedicated regression test for "write to the same spool
  directory twice with `--erase`".
- `MTERASE` remains a no-op in `src/neotape_tape.cpp:661-662`, although the
  current `neotape-write --erase` path no longer depends on it.

### C5. F5 bracketed IPv6 literals are now normalized before runtime resolution

Status: fixed in implementation

Evidence:

- `parse_address()` now strips `[` and `]` from IPv6 literals in
  `src/neotape_tcp_protocol.cpp:145-149`.
- The parser unit test now expects `tcp://[::1]:9123` to normalize to host
  `::1` in `tests/test_tcp_protocol.cpp:154-163`.

Residual risk:

- There is still no integration test that performs a real bind/connect round
  trip using the documented bracketed IPv6 form.

## Validation performed

- `make -j4`
- `bin/test_format`
- `bin/test_validate`
- `bin/test_tcp_protocol`
- `sh tests/smoke_tcp_archive_multi.sh`
- `sh tests/smoke_tcp_extract.sh`
- `sh tests/smoke_inspect.sh`
- `git diff --check`
