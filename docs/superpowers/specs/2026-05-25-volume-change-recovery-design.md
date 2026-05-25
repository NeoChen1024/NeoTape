# Volume Change Recovery Design

## Goal

Make NeoTape real-tape volume change behavior explicit and safe for both write
and restore paths before running destructive `/dev/nst0` validation. The design
must prevent silent success on EOT, reject wrong volumes, and avoid continuing a
restore after frame or slice sequence corruption.

## Context

Current code already has a shared interactive prompt API in
`include/neotape/cli.hpp` and `src/neotape_cli.cpp`:

- `neotape::prompt_for_volume_change()` supports continue, change locator,
  shell, and abort.
- `neotape::require_prompt_allowed()` enforces `--control=none`.

The real tape writer in `src/neotape_tape_writer.cpp` does not use that shared
prompt. It uses a simple `prompt_next_volume()` that waits for Enter, does not
support abort, does not support changing device, and does not honor
`--control=none`.

The real tape restore path in `src/neotape_cat_volumes.cpp` reads one tape
volume and fails if no Archive End Header is found. It preserves frame and slice
state inside `VolumeReadState`, but it does not prompt for a next tape volume.

## Non-Goals

- Do not implement automatic salvage.
- Do not implement automatic scan to the next matching Volume Header on the same
  tape.
- Do not add the full Phase 7 policy matrix yet, such as
  `--on-mismatch=scan-next-volume-header` or retry counts.
- Do not continue restore after frame or slice sequence mismatch.

## Control Policy

NeoTape keeps the existing CLI surface for this work:

- `--control=auto` allows `/dev/tty` prompts.
- `--control=none` fails immediately when user intervention would be required.

No new command-line flags are required for this implementation.

## Prompt Model

All volume-change prompts use `neotape::prompt_for_volume_change()`.

The prompt request must include enough identity to make the operator decision
safe:

- current archive UUID
- expected volume sequence number
- current locator
- read/write mode

For mismatch diagnostics, stderr must include expected and actual values before
prompting again. Diagnostics must not be written to stdout because restore stdout
may contain payload bytes.

Supported prompt outcomes:

- `continue_current`: retry the current device or current position after the
  operator has intervened.
- `change_locator`: close the old tape handle, open the replacement locator, and
  retry using that device.
- `abort`: throw an error and exit non-zero.
- `shell`: handled inside the existing prompt implementation; after the shell
  exits, the prompt is shown again.

## Writer Behavior

### EOT And Short Write

When a tape record write returns ENOSPC or a short write, the writer treats that
as a volume-change requirement.

With `--control=none`:

- fail immediately
- include the archive UUID and expected next volume sequence number in stderr
- do not report archive success

With `--control=auto`:

- flush diagnostics to stderr
- call `prompt_for_volume_change()`
- on `continue_current`, retry the failed write
- on `change_locator`, open the replacement `tape:<device>` read-write, rewind
  it, configure block mode, then write a new Volume Header before retrying
  content or Archive End records
- on `abort`, fail non-zero

### New Volume Header

After a successful volume change, the writer writes the next Volume Header using
the same archive UUID and archive name, with `volume_seq_num` incremented by one.
The writer does not append to unrelated existing archive data on the replacement
volume.

### Wrong Or Non-Empty Replacement Media

If replacement media preflight detects a non-blank or incompatible NeoTape
medium and strict append rules cannot safely continue:

- `--control=none` fails
- `--control=auto` reports the mismatch and prompts again

This implementation does not scan inside that media for a usable append point.

## Restore Behavior

### Expected Next Volume

Restore must carry expected archive identity across volumes:

- archive UUID from the first accepted Volume Header
- archive name from the first accepted Volume Header
- payload profile from the first accepted Volume Header
- volume block size from the first accepted Volume Header
- expected next `volume_seq_num`

For the first volume, the Volume Header establishes this identity. For each
later volume, the Volume Header must match it.

### Volume Header Validation

Before reading frames from a new tape volume, restore validates:

- actual `archive_uuid` equals expected archive UUID
- actual `volume_seq_num` equals expected volume sequence number
- actual `volume_block_size` equals expected volume block size
- actual `payload_profile` equals expected payload profile

If validation fails, restore must not process any frames from that volume.

With `--control=none`:

- fail immediately with expected and actual identity in stderr

With `--control=auto`:

- report expected and actual identity in stderr
- prompt for operator action
- retry or open another locator according to the prompt result

This covers two important operator errors:

- the operator inserted a volume from the wrong archive
- the operator inserted a tape that contains volume 2 for another archive
  instance on the same medium

NeoTape must not accept a Volume Header merely because its sequence number is
the expected value. Archive UUID must match.

### Missing Archive End

If the current volume ends without an Archive End Header and no slice is open,
restore treats this as a clean volume boundary and requests the next volume.

With `--control=none`:

- fail with `archive incomplete: no Archive End Header found`

With `--control=auto`:

- prompt for next volume
- validate the next Volume Header before reading frames
- continue using the same `VolumeReadState`

If the current volume ends while a slice is open, restore fails. The output may
already contain partial payload bytes, and this design does not implement salvage
or rollback.

## Frame And Slice Sequence Mismatch

Restore must reject frame or slice sequence mismatch as stream corruption, not as
a normal volume-change event.

The existing read state must remain authoritative:

- expected `global_frame_seq_num`
- expected `logical_slice_seq_num`
- whether a slice is currently open
- accumulated slice byte count
- accumulated slice BLAKE3 hash

Every frame must satisfy:

- frame archive UUID matches the accepted archive UUID
- frame volume sequence number matches the accepted current volume
- frame volume block size matches the accepted block size
- `global_frame_seq_num` equals the expected global frame sequence number
- a slice start frame has the expected logical slice sequence number
- a slice end frame has matching `slice_content_size`
- a slice end frame has matching `slice_content_blake3`

On mismatch:

- `--control=none` fails immediately
- `--control=auto` fails immediately after printing a clear diagnostic

The prompt must not offer normal continue/change-device recovery for frame or
slice mismatch in this implementation. A future salvage or scan policy can add
explicit operator-controlled recovery, but automatic continuation is unsafe.

## Data Flow

Writer path:

1. CLI parses `--control` into `BackupOptions` or raw write options.
2. `src/neotape_write.cpp` copies that policy into `mt::TapeWriterOptions`.
3. `src/neotape_tape_writer.cpp` detects ENOSPC or short write.
4. The writer asks `prompt_for_volume_change()` unless `--control=none` forbids
   it.
5. The writer opens or retries a tape device and writes the next Volume Header.
6. The failed record is retried.

Restore path:

1. CLI parses `--control` into `RawReadOptions`.
2. `src/neotape_cat_volumes.cpp` opens the first tape locator.
3. The first Volume Header establishes restore identity.
4. `process_volume()` validates every frame and updates `VolumeReadState`.
5. Missing Archive End at a clean volume boundary triggers the shared prompt.
6. The next candidate Volume Header is validated before any payload is emitted
   from that volume.

## Error Messages

Volume identity mismatch diagnostics should include:

- expected archive UUID
- expected volume sequence number
- actual archive UUID
- actual volume sequence number
- actual payload profile when available
- actual block size when available

Frame and slice mismatch diagnostics should include:

- current volume sequence number
- expected and actual global frame sequence number when applicable
- expected and actual logical slice sequence number when applicable
- reason for rejecting continuation

## Testing

Tests should avoid requiring real tape hardware.

Writer tests:

- use a test double or spool-backed device to force ENOSPC or short write
- verify `--control=none` fails without prompting
- verify `--control=auto` can abort and exits non-zero
- verify replacement device flow writes the next Volume Header with the same
  archive UUID and incremented volume sequence number

Restore tests:

- build small spool or test-double volumes with known Volume Headers and frames
- verify missing Archive End prompts or fails according to control policy
- verify wrong archive UUID is rejected before frame processing
- verify wrong `volume_seq_num` is rejected before frame processing
- verify same sequence number from another archive instance is rejected
- verify global frame sequence mismatch fails
- verify logical slice sequence mismatch fails
- verify slice hash mismatch fails

Manual real-tape validation:

- keep the existing `/dev/nst0` validation plan
- ensure the plan tests `--control=auto` prompt behavior at EOT
- add negative manual cases for wrong volume and same-medium wrong archive volume
  when suitable test media are available

## Open Implementation Notes

- The current `VolumePromptChoice::shell` enum value is not returned directly by
  `prompt_for_volume_change()` because the shell loop is handled internally.
  Callers should still handle unexpected values defensively.
- Existing prompt text may be extended, but stdout must remain payload-clean.
- Replacement locators for this work should be `tape:<device>`. Spool recovery
  can remain limited to existing spool code paths.

## Self-Review

- Placeholder scan: no placeholder requirements remain.
- Internal consistency: writer and restore both use existing `--control` and the
  shared prompt API.
- Scope check: this is focused on safe prompt, identity validation, and mismatch
  rejection; scan and salvage policies are intentionally excluded.
- Ambiguity check: wrong volume, wrong archive instance, frame mismatch, and
  slice mismatch each have explicit behavior.
