# Error Handling

Status: normative.

## Policy Model

NeoTape uses a Retry / Inspect / Fail / Force-Salvage error policy model. All error handling operates on stderr and `/dev/tty`; stdout must never contain error messages, prompts, or diagnostics.

## Error Classes

### Frame Mismatch or Corruption

- Default: do not emit payload bytes from that frame.
- Interactive: prompt with Retry, Inspect, ScanNextValidFrame, Fail.
- `ScanNextValidFrame` MUST advance by tape-file/filemark boundaries and only accept a candidate whose magic, `channel_type`, `frame_hash`, and sequence numbers validate.
- This option is essential on multi-archive media where a candidate may belong to another archive.
- Force is available in salvage mode only, and MUST mark output as not fully verified.

### UUID Mismatch

- Default: do not continue.
- Interactive: prompt with Retry, Inspect, ScanNextArchive, Fail.
- Force only in salvage mode.

### Frame Sequence Mismatch

- Default: fail or prompt retry.
- In salvage mode: attempt to resynchronize within the slice tape file using `frame_payload_size`. If resync fails, seek to the next slice-level filemark.

### Frame Hash Mismatch

- Default: fail. The frame is corrupt.
- In salvage mode: skip the frame and log a warning.
- `ch_metadata` frame hash failures: SHOULD log a warning and continue.

### Read Error

- Automatic retry N times (`--retry=N`).
- On persistent failure: prompt Retry / Inspect / Fail.
- Skip damaged block is available in salvage mode only, with a clear warning about payload stream corruption.

### EOT Before Archive End Frame

- If a frame was committed but fewer than `frame_payload_size` payload bytes were written: the frame is incomplete. The next volume must restart the same logical slice with a new frame covering the remaining payload range.
- If the Archive End frame was not committed: the archive is not cleanly complete. The next volume must complete remaining slices and write the Archive End frame.

## Control Plane

### Output Channels

| Channel | Content |
|---------|---------|
| stdout | `ch_content` payload bytes only. |
| stderr | Log messages, progress, warnings. |
| `/dev/tty` | Interactive prompts (volume change, Retry/Inspect/Fail). |

### Control Modes

| Mode | Behavior |
|------|----------|
| `--control=auto` | Use `/dev/tty` if available, otherwise follow policy flags. |
| `--control=tty` | Require `/dev/tty`. Fail if unavailable. |
| `--control=none` | No interaction. Suitable for cron/systemd/CI/robot changers. |

### Error Policy Flags

| Flag | Values | Description |
|------|--------|-------------|
| `--on-mismatch` | `prompt`, `fail`, `scan-next-archive` | UUID/mismatch policy. |
| `--on-header-error` | `prompt`, `fail`, `scan-next-valid` | Corrupt header policy. |
| `--on-eot` | `prompt`, `fail` | EOT handling policy. |
| `--retry=N` | integer | Read retry count. |
| `--salvage` | flag | Enable force options and best-effort extraction. |

## Salvage Mode

In salvage mode, the reader MAY:
- Emit `ch_content` payload bytes from archive volumes with frame errors (with warning).
- Skip corrupt frames or blocks within a slice.
- Attempt frame-level resync within a slice tape file.
- Skip to the next slice-level filemark when slice-internal resync fails.

Salvage output MUST be explicitly marked as not fully verified. The reader SHOULD report which slices or frames were affected.

## Reader Validation Priority

A normal reader validates in order:

1. Complete NeoTape record is read (`volume_block_size_kib * 1024` bytes).
2. 512-byte fixed header has valid magic and version.
3. `channel_type` is recognized.
4. `frame_payload_size <= (volume_block_size_kib * 1024) - 512`.
5. `frame_hash` validates for each frame.
6. `archive_uuid` and sequence number continuity hold.
7. Archive completion is declared only by a valid Archive End frame with `CLEAN_END` set.

If readback reaches EOD before a valid Archive End frame, the archive is incomplete even if all prior frames are valid.
