# Error Handling

Status: normative.

## Policy Model

NeoTape uses a Retry / Inspect / Fail / Force-Salvage error policy model. All error handling operates on stderr and `/dev/tty`; stdout must never contain error messages, prompts, or diagnostics.

## Error Classes

### Volume Header Mismatch or Corruption

- Default: do not emit payload bytes from that candidate volume.
- Interactive: prompt with Retry, Inspect, ScanNextVolumeHeader, Fail.
- `ScanNextVolumeHeader` MUST advance by tape-file/filemark boundaries and only accept a candidate whose magic, type, CRC32C, `archive_uuid`, and `volume_seq_num` all validate.
- This option is essential on multi-archive media where a candidate may be the tail of another archive.
- Force is available in salvage mode only, and MUST mark output as not fully verified.

### UUID Mismatch

- Default: do not continue.
- Interactive: prompt with Retry, Inspect, ScanNextVolumeHeader, Fail.
- Force only in salvage mode.

### Volume Sequence Mismatch

- Default: prompt for the correct volume.
- If a higher `volume_seq_num` is found, a volume may be missing.
- If a lower or equal `volume_seq_num` is found, the wrong volume is inserted or the tape was rewound.

### Frame Sequence Mismatch

- Default: fail or prompt retry.
- In salvage mode: attempt to resynchronize within the slice tape file using `frame_payload_size`. If resync fails, seek to the next slice-level filemark and look for the next slice's first Frame Header.

### Read Error

- Automatic retry N times (`--retry=N`).
- On persistent failure: prompt Retry / Inspect / Fail.
- Skip damaged block is available in salvage mode only, with a clear warning about payload stream corruption.

### EOT Before Frame or Archive End Header

- If a Frame Header was committed but fewer than `frame_payload_size` payload bytes were written: the Frame is incomplete. The next volume must restart the same logical slice with a new Frame covering the remaining payload range.
- If the writer completed a slice but the END Frame Header was not committed: the next volume must complete the slice with an END Frame before writing the slice-level filemark.
- If an END Frame Header is found but the slice-level filemark or Archive End Header is absent: the archive is not cleanly complete. The next tape file may be the next logical slice's first Frame Header or the Archive End Header.

## Control Plane

### Output Channels

| Channel | Content |
|---------|---------|
| stdout | Payload bytes only. |
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
| `--on-mismatch` | `prompt`, `fail`, `scan-next-volume-header` | UUID/mismatch policy. |
| `--on-volume-header-error` | `prompt`, `fail`, `scan-next-volume-header` | Corrupt header policy. |
| `--on-eot` | `prompt`, `fail` | EOT handling policy. |
| `--retry=N` | integer | Read retry count. |
| `--salvage` | flag | Enable force options and best-effort extraction. |

## Salvage Mode

In salvage mode, the reader MAY:
- Emit payload bytes from volumes with header errors (with warning).
- Skip corrupt Frames or blocks within a slice.
- Attempt Frame-level resync within a slice tape file.
- Skip to the next slice-level filemark when slice-internal resync fails.

Salvage output MUST be explicitly marked as not fully verified. The reader SHOULD report which slices, Frames, or byte ranges were affected.

## Reader Validation Priority

A normal reader validates in order:

1. Complete `volume_block_size` record is read.
2. 1024-byte fixed header has valid magic, version, type, and CRC32C.
3. `frame_payload_size <= volume_block_size - 1024`.
4. `frame_payload_blake3` validates for each Frame.
5. Slice-level `slice_content_blake3` validates for each slice (from END Frame Header).
6. Archive completion is declared only by a valid Archive End Header with `CLEAN_END` set.

If readback reaches EOD before a valid Archive End Header, the archive is incomplete even if all prior Frames are valid.
