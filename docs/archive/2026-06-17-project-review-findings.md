# Project implementation review findings

Date: 2026-06-17

## Review scope

This review focused on the current end-to-end archive pipeline:

- frame format and validator
- TCP server/client protocol flow
- `neotape-archiver`, `neotape-raw-store`, `neotape-write`, `neotape-read`
- `neotape-extractor`, `neotape-inspect`
- spool/tape backend behavior

Validation performed during review:

- `make -j4`
- `bin/test_format`
- `bin/test_tcp_protocol`
- `bin/test_pax_pipeline`
- `sh tests/smoke_raw_store.sh`
- `sh tests/smoke_tcp_archive.sh`
- `sh tests/smoke_tcp_archive_multi.sh`
- `sh tests/smoke_tcp_extract.sh`
- `sh tests/smoke_tcp_extract_multi.sh`
- `sh tests/smoke_inspect.sh`
- `sh tests/smoke_mt_pax_pipeline.sh`
- `sh tests/smoke_mt_pax_parity.sh`

The current tree builds and the existing tests pass, but the findings below
show several spec-compliance and robustness gaps that are not covered today.

## Findings

### F1. `neotape-archiver` emits spec-invalid multi-frame content groups

Severity: high

Impact:

- Any pax archive slice that spans more than one NeoTape frame is emitted as a
  sequence of single-frame `ch_content` groups instead of one contiguous
  channel group.
- This violates the frame/channel model in the spec and weakens interoperability
  with stricter readers or future validator improvements.

Evidence:

- `src/neotape_tcp_server.cpp:219-229` sets
  `frame_seq_num_within_channel = 1` and `flags = START | END` for every
  content frame emitted by `FrameBuilder::build_frame()`.
- `docs/spec/03-frames-and-slices.md:19-30` and
  `docs/spec/03-frames-and-slices.md:51-75` require one logical slice to be a
  contiguous channel group whose `frame_seq_num_within_channel` increments
  across frames, with `START` only on the first frame and `END` only on the
  last frame.
- `src/neotape_raw_store_cmd.cpp:307-338` already implements the correct
  behavior for the raw-store path, which makes this drift inside the same
  project especially visible.

Suggested fix:

- Make `FrameBuilder` maintain per-slice content-group state:
  `frame_seq_num_within_channel`, `START` on first frame, `END` only when the
  slice closes.
- Reuse the same frame-group builder logic in both archiver and raw-store
  instead of keeping two separate implementations.

Missing test coverage:

- `tests/smoke_tcp_archive_multi.sh:54-89` checks channel type and archive-end
  presence, but does not assert `frame_seq_num_within_channel` continuity or
  `START`/`END` placement on multi-frame slices.

### F2. The extractor does not implement advisory metadata semantics

Severity: high

Impact:

- `neotape-extractor` currently writes metadata-channel payload bytes into the
  reconstructed output stream.
- A corrupt `ch_metadata` frame causes extraction to fail, even though the spec
  says metadata is advisory and should not block restore.

Evidence:

- `src/neotape_extractor.cpp:174-180` appends payload bytes from every frame
  without checking `channel_type`.
- `src/neotape_extractor.cpp:153-156` aborts extraction on any validator error.
- `src/neotape_validate.cpp:27-31` verifies `frame_hash` unconditionally for
  all channels.
- `docs/spec/03-frames-and-slices.md:44-50` says a normal payload reader MUST
  emit only `ch_content` bytes and SHOULD continue when `ch_metadata` is
  missing, truncated, or corrupt.

Suggested fix:

- In extractor mode, only append payload for `ChannelType::CH_CONTENT`.
- Treat metadata validation failures as warnings in restore mode:
  hash failure, missing metadata, or bad metadata ordering should not poison the
  restored content stream.
- Keep strict archive validation in `neotape-inspect`, but split that policy
  from the extractor's restore-mode behavior.

Missing test coverage:

- There is no smoke or unit test that injects `ch_metadata` frames and verifies
  that extraction output excludes them.
- There is no test for "corrupt metadata frame, valid content frame" restore
  behavior.

### F3. `spool:` targets do not honor `--erase`

Severity: high

Impact:

- `neotape-write --target spool:... --erase` does not actually clear prior spool
  contents.
- Old finalized `.nts` files remain in place and new output starts at the next
  file number, so an operator asking for overwrite gets append-like behavior
  instead.

Evidence:

- `src/neotape_write_cmd.cpp:548-552` uses `rewind()` for `--erase`.
- `src/neotape_tape.cpp:433-454` opens a new pending spool file after the last
  existing finalized tape file.
- `src/neotape_tape.cpp:598-606` implements `MTREW` for `SpoolTapeDevice` as a
  plain `lseek()` on the current pending file; it does not remove old `.nts`
  files, reset `files_`, or reset numbering to BOT.
- `src/neotape_tape.cpp:625-626` implements `MTERASE` as a no-op for spool.

Suggested fix:

- Define BOT/EOD semantics for spool explicitly and implement them:
  `--erase` should remove existing finalized spool files and reset numbering.
- Make spool `rewind()` and/or `erase()` reflect those semantics so the CLI path
  does not need backend-specific special cases.

Missing test coverage:

- No smoke test exercises a second write into the same spool directory with
  `--erase` and asserts that old files are removed.

### F4. `FrameValidator` and `neotape-inspect` under-enforce structure around channel boundaries and `archive_end`

Severity: medium

Impact:

- Malformed archives can slip through compliance reporting when sequence numbers
  happen to remain monotonic.
- The current validator uses `START`/`END` mostly as hints for sequence resets,
  but does not require them where the spec makes them normative.
- `archive_end` is documented as the final frame, yet the validator relies on
  caller discipline rather than enforcing that invariant itself.

Evidence:

- `include/neotape/validate.hpp:22-23` documents that `archive_end` must be the
  final frame.
- `include/neotape/validate.hpp:47-48` says callers must stop after
  `saw_archive_end == true`.
- `src/neotape_validate.cpp:16-158` has no early guard rejecting frames fed
  after `saw_archive_end`.
- `src/neotape_validate.cpp:133-155` checks channel-local sequence continuity,
  but does not require `START` on a new group or `END` before a slice/channel
  transition.
- `src/neotape_inspect_cmd.cpp:454-537` keeps feeding frames to the validator
  until physical EOD, even after `archive_end` has been seen.

Suggested fix:

- Enforce `START` on the first frame of a new `(slice, channel)` group.
- Enforce `END` before a channel-group or slice transition.
- Reject any frame presented after `archive_end`.
- Consider giving the validator a stricter "inspect/compliance mode" so the
  extractor can stay more permissive where the spec allows it.

Missing test coverage:

- `tests/smoke_inspect.sh:54-108` only asserts a passing report on a valid
  archive.
- There are no negative inspect cases for missing `START`.
- There are no negative inspect cases for missing `END`.
- There are no negative inspect cases for extra frames after `archive_end`.
- There are no negative inspect cases for metadata-after-content ordering
  failures.

### F5. Documented bracketed IPv6 addresses are parsed but not normalized for runtime resolution

Severity: medium

Impact:

- The documented and unit-tested form `tcp://[ipv6]:port` is accepted by the
  parser, but the host string is left bracketed and then handed directly to
  `getaddrinfo()`.
- Standard resolver APIs expect the raw IPv6 literal without brackets, so the
  documented form is not robust at runtime.

Evidence:

- `include/neotape/tcp_protocol.hpp:73-82` documents
  `tcp://[ipv6]:port` as a supported form.
- `src/neotape_tcp_protocol.cpp:136-145` stores the host substring exactly as
  written, including brackets.
- The various listener/connect paths pass that host directly to
  `getaddrinfo()`: `src/neotape_tcp_server.cpp:61-84`,
  `src/neotape_extractor.cpp:69-92`, `src/neotape_write_cmd.cpp:197-215`,
  `src/neotape_read_cmd.cpp:136-154`, and
  `src/neotape_raw_store_cmd.cpp:206-229`.
- `tests/test_tcp_protocol.cpp:153-161` only tests parser output; it does not
  exercise a real bind/connect using the bracketed form.

Suggested fix:

- Normalize bracketed IPv6 literals inside `parse_address()` or immediately
  before `getaddrinfo()`.
- Add an integration test that actually binds/connects with
  `tcp://[::1]:<port>`.

## Cross-cutting architecture note

The TCP protocol plumbing is currently duplicated across several executables:

- listener creation
- UDS/TCP connect logic
- `send_error()` helpers
- retention window handling
- frame hashing / record patching helpers

This duplication has already drifted into behavioral inconsistency:

- `src/neotape_raw_store_cmd.cpp:307-338` has correct multi-frame channel-group
  state
- `src/neotape_tcp_server.cpp:219-229` does not

A worthwhile cleanup would be to extract:

- one shared socket/listener utility
- one shared frame-group builder
- one shared archiver/raw-store server state machine

That should reduce future protocol drift and make new invariants easier to test
once instead of in multiple CLI entrypoints.
