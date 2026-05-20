# LTO Behavior Notes

Status: empirical notes / implementation guidance.

This document records observed LTO/Linux tape behavior that affects NeoTape
writer policy. These notes are not a replacement for the format specification;
they are backend behavior observations used to choose conservative writer rules.

## Scope

The observations below come from an experimental Frame-per-record EOT probe that
wrote NeoTape-like Frame records to a deliberately small tape partition and then
read the records back for verification.

The probe used complete records of the selected `volume_block_size`. Each record
contained a 1024-byte Frame-like header followed by deterministic PRNG payload
bytes and padding to the record size. Readback validated the header CRC32C, the
probe payload digest, and regenerated PRNG payload bytes.

The probe was not a stable NeoTape archival writer. Its purpose was to observe
real tape behavior near EOT/EOM.

## Observed Result Summary

No partial Frame was observed in the tested environment.

For all tested record sizes:

- Successful writes returned exactly `volume_block_size` bytes.
- Failed writes returned `-1` with `errno = ENOSPC`.
- No positive short write was observed.
- Readback produced exactly the same number of valid Frames as successful
  complete writes.
- No invalid header, invalid payload, or partial record was observed during
  readback.
- Readback ended with zero-length reads after the final valid Frame.

Observed test sizes:

| `volume_block_size` | Successful complete writes | Failed writes | Positive short writes | Valid Frames read back | Invalid or partial reads |
| --------------------- | -------------------------: | ------------: | --------------------: | ---------------------: | -----------------------: |
| 64 KiB                |                     486149 |            33 |                     0 |                 486149 |                        0 |
| 1 MiB                 |                      33164 |            33 |                     0 |                  33164 |                        0 |
| 4 MiB                 |                       8713 |            33 |                     0 |                   8713 |                        0 |

These results support using a complete NeoTape Frame record as the smallest
writer commit unit. However, NeoTape MUST NOT rely on an unconditional hardware
atomic-write guarantee. Backend implementations must still treat any short
write, residual write, synchronization error, or unknown write status as an
uncommitted Frame.

## ENOSPC Pattern Near EOT

Near EOT/EOM, the probe observed an alternating pattern where a complete write
could succeed after a prior `ENOSPC` result. This is consistent with the idea
that an early-warning region can still allow limited trailer-like writes after
an `ENOSPC` indication.

NeoTape MUST NOT use this post-`ENOSPC` region for ordinary archive payload
content.

## Writer Policy

The tape backend SHOULD use the following conservative policy:

1. A Frame is considered tentatively committed only if `write()` returns exactly
   `volume_block_size`.
2. If `write()` returns a positive value smaller than `volume_block_size`, the
   attempted Frame MUST be treated as uncommitted.
3. If `write()` returns `-1` with `errno = ENOSPC`, the attempted Frame MUST be
   treated as uncommitted.
4. After the first `ENOSPC` while writing ordinary archive content, the writer
   MUST stop writing ordinary content Frames to the current volume.
5. After the first `ENOSPC`, the writer SHOULD switch to volume-change handling:
   close or synchronize the current tape position as appropriate, request the
   next volume, write the next Volume Header, and resume with the first
   uncommitted Frame.
6. The writer MAY use a small backend-specific reserve area after early-warning
   EOT only for required close-out or synchronization operations, not for normal
   payload content.
7. If a synchronization point reports a deferred write error, all Frames whose
   commit status depends on that synchronization point MUST be treated as
   uncommitted unless the backend can prove otherwise.

In short: the first `ENOSPC` is a volume-change trigger for normal NeoTape
content, even if later writes might still succeed physically.

## Reader Policy

The reader should not use EOT/EOD status alone as the archive completion rule.
Format-level validation remains authoritative.

A normal reader should validate, in order:

1. A complete `volume_block_size` record was read.
2. The 1024-byte fixed header has valid magic, version, type, and CRC32C.
3. `frame_payload_size <= volume_block_size - 1024`.
4. The Frame payload hash validates.
5. Slice-level integrity validates when the relevant content group ends.
6. Archive completion is declared only by a valid Archive End Header with the
   clean-completion flag set.

If readback reaches EOD/EOT or repeated zero-length reads before a valid Archive
End Header, the archive instance is incomplete even if all preceding Frames are
valid.

## Specification Implication

NeoTape should define Frame-level commit semantics without claiming that LTO or
any operating system tape stack guarantees atomic record writes.

Recommended normative direction:

```text
A Frame is the smallest NeoTape commit unit.

A writer MUST treat a Frame as committed only after the backend reports a
complete write of the entire NeoTape record and no deferred write error is
reported at the next synchronization point.

A partial write, short write, residual write, failed synchronization, or unknown
write status MUST cause the attempted Frame to be treated as uncommitted.

After the first ENOSPC while writing ordinary content, the tape backend MUST NOT
continue writing ordinary content Frames to the current volume. It MUST enter
volume-change handling and resume with the first uncommitted Frame on the next
volume.
```


## Additional Notes:

Tested native capacity of non-partitioned LTO-5 tapes:
`partition number: 0, partition record data counter [MB]: 1541438`
