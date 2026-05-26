# Read And Restore Volume Changer Design

## Goal

Restore NeoTape volume changer behavior for reader-side commands and verify that
writer-side commands still use the shared changer path. The affected commands are
`neotape read`, `neotape restore`, `neotape write`, and `neotape backup`.

## Context

NeoTape already has shared prompt primitives in `include/neotape/cli.hpp` and
`src/neotape_cli.cpp`:

- `parse_locator()` parses `tape:<device>` and `spool:<dir>` locators.
- `parse_control_policy()` parses `--control=auto|none`.
- `require_prompt_allowed()` fails before a prompt when `--control=none` is set.
- `prompt_for_volume_change()` lets the operator continue, change locator, enter
  a shell, or abort.

The tape writer path in `src/neotape_tape_writer.cpp` already uses these
primitives through `handle_volume_change()`. `neotape backup --target tape:` and
raw `neotape write --target tape:` both populate `TapeWriterOptions::control`.

The reader path in `src/neotape_read_cmd.cpp` is inconsistent:

- `neotape restore --source tape:` has a local prompt loop.
- `neotape read --source tape:` is rejected by argument parsing.
- `neotape read` and spool `neotape restore` fail on an incomplete archive
  instead of asking for the next volume.

## Requirements

`neotape read` and `neotape restore` must support the same changer behavior for
both `tape:` and `spool:` sources.

For `tape:` sources:

- Open the initial `tape:<device>` locator read-only.
- Process one tape volume at a time using `TapeDeviceVolumeReader`.
- If `ArchiveEndHeader` is reached, finish normally.
- If the volume ends without `ArchiveEndHeader` and no slice is open, request the
  next volume through `prompt_for_volume_change()`.
- On continue, reopen the current device and retry from the current locator.
- On change locator, accept only `tape:<device>`, open it read-only, and continue.
- On abort, fail non-zero.

For `spool:` sources:

- Open the initial `spool:<dir>` locator.
- Process the volume represented by that directory.
- If `ArchiveEndHeader` is reached, finish normally.
- If the volume ends without `ArchiveEndHeader` and no slice is open, request the
  next volume through `prompt_for_volume_change()`.
- On continue, reopen the current spool directory and retry.
- On change locator, accept only `spool:<dir>`, open it, and continue.
- On abort, fail non-zero.

For all reader-side prompts:

- Flush the output sink before prompting.
- Keep stdout payload-clean; diagnostics and prompts stay on stderr or `/dev/tty`.
- Honor `--control=none` by failing with the existing
  `volume change required but --control=none is set` message before prompting.
- Reject unsupported replacement locator kinds with a clear error.

For writer-side commands:

- Keep `neotape write --target tape:` passing `--control` into
  `TapeWriterOptions`.
- Keep `neotape backup --target tape:` passing `--control` into
  `TapeWriterOptions`.
- Do not add volume changer behavior to spool writes in this change; spool writes
  create files in a filesystem backend and do not prompt for removable media.

## Non-Goals

- Do not add automatic scanning for a matching Volume Header.
- Do not implement a JSON control protocol.
- Do not add retry limits or Phase 7 policy flags.
- Do not attempt rollback when a volume ends with an open slice; keep failing.
- Do not change prompt UI except as needed to preserve existing data in
  `VolumePromptRequest`.

## Design

Add a small reader-side changer helper in `src/neotape_read_cmd.cpp` rather than
duplicating prompt code in spool and tape loops.

The helper should take:

- current locator
- expected archive UUID from `VolumeReadState::validation`
- expected volume sequence number from `VolumeReadState::validation`
- command options, including `--control`
- an output sink to flush before prompting

It should return the next locator after applying the operator choice. Callers are
responsible for opening the returned locator and processing that volume.

Reader orchestration should be split by source kind but use the same state model:

1. Create `VolumeReadState` once for the whole archive.
2. Process the current volume with `process_volume()`.
3. Stop when `process_volume()` returns true.
4. Fail if `rs.validation.slice_open` remains true at volume boundary.
5. Ask the changer helper for the next locator.
6. Repeat with the same `VolumeReadState`.

`neotape read` should parse tape sources the same way `neotape restore` does.
Both commands still share `RawReadOptions`; the output command name only affects
the final pax end-of-archive padding appended by `restore`.

## Validation

Use existing validation in `restore_validation` for archive identity and sequence
checks. This work does not weaken frame, slice, or archive-end validation.

Replacement volumes are validated naturally when their Volume Header is accepted
by `process_volume()` and `restore_validation`. A wrong replacement volume fails
before payload frames are accepted.

## Tests

Add regression tests that do not require real tape hardware.

Reader tests:

- `neotape read --source tape:<device-like-test-double> --control=none` reaches
  the volume-change-required path when no Archive End Header is present.
- `neotape read --source spool:<dir> --control=none` reaches the same path for an
  incomplete spool archive.
- `neotape restore --source spool:<dir> --control=none` reaches the same path for
  an incomplete spool archive.

Writer tests:

- Preserve existing tests proving tape writer `--control=none` fails before an
  interactive prompt on impossible writes.
- If needed, add a CLI-level assertion that raw tape write and tape backup copy
  `--control` into `TapeWriterOptions`.

Full verification should run `make test`. If focused tests are available, run the
smallest relevant target first, then the full suite.

## Self-Review

- Placeholder scan: no TODO, TBD, or open-ended requirements remain.
- Internal consistency: reader-side commands share one changer behavior, while
  writer-side commands keep the existing tape writer changer.
- Scope check: this is one implementation slice focused on read/restore changer
  restoration plus write/backup verification.
- Ambiguity check: tape and spool replacement locator rules, `--control=none`,
  open-slice failure, and stdout cleanliness are explicit.
