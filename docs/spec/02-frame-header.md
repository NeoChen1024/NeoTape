# Frame Header Layout

Status: normative.

This chapter is the authoritative source for the exact on-wire Frame Header
layout, field widths, field order, and channel/flag assignments. Other
chapters may reference header fields conceptually, but they MUST defer to this
chapter for exact sizes and positions.

All NeoTape records use a single unified 512-byte fixed header. The final 32 bytes are `frame_hash`, a BLAKE3 digest over the canonical image of the entire frame.

## Fixed Fields

| Field                      | datatype       | size (in bytes) | Requirement | Notes                                                                                                      |
| -------------------------- | -------------- | --------------- | ----------- | ---------------------------------------------------------------------------------------------------------- |
| `magic`                  | `char[8]`    | 8               | MUST        | Fixed NeoTape identifier:`NeoTape\0`.                                                                    |
| `header_version`         | `uint8`      | 1               | MUST        | Currently`1`; bump on layout-breaking changes.                                                           |
| `channel_type`           | `uint8_enum` | 1               | MUST        | Frame role. See[Channel Types](#channel-types).                                                             |
| `volume_block_size_kib`  | `uint16`     | 2               | MUST        | Record size for this volume, encoded in KiB.                                                               |
| `archive_uuid`           | `nt_uuid`    | 37              | MUST        | NUL-terminated UUID string.                                                                                |
| `archive_label`          | `nt_name`    | 65              | SHOULD      | NUL-terminated/padded human-readable archive label.                                                        |
| `volume_seq_num`         | `uint64`     | 8               | SHOULD      | Advisory volume sequence number; part of the volume label/name.                                            |
| `global_frame_seq_num`   | `uint64`     | 8               | MUST        | Monotonically increasing across**all** frames, including `archive_end`.                            |
| `slice_seq_num`          | `uint64`     | 8               | MUST        | `0` for the `archive_end` control frame.                                                               |
|  `channel_frame_seq_num` | `uint64`     | 8               | MUST        | `0` for the `archive_end` control frame.                                                               |
| `frame_payload_size`     | `uint32`     | 4               | MUST        | Meaningful payload bytes after the fixed header.                                                           |
| `flags`                  | `uint64`     | 8               | MUST        | Frame flags. See[Flags](#flags).                                                                            |
| `_reserved`              | `byte[122]`  | 122             | MUST        | Zero-filled padding. Immediately followed by `sideband_data` at offset 280.                               |
| `sideband_data`          | `byte[128]`  | 128             | MAY         | Optional sideband data. Interpretation is defined by `channel_type`. See [Sideband Data](#sideband-data). |
| `signature`              | `byte[72]`   | 72              | MAY         | 8-byte key ID plus 64-byte Ed25519 signature over`NeoTape-frame\0 \|\| frame_hash` when `SIGNED` is set. |
| `frame_hash`             | `nt_hash`    | 32              | MUST        | BLAKE3 over the canonical image of the whole frame.                                                        |

Total: 512 bytes.

Datatype rules from `docs/spec/00-format-common.md` still apply (little-endian integers, NUL-terminated strings, BLAKE3 algorithm, etc.).

## Field Semantics

### `volume_block_size_kib`

Stores the NeoTape record size in KiB, not bytes. The decoded record size is `volume_block_size_kib * 1024` bytes. Writers MUST encode only whole-KiB record sizes; readers MUST reject `0` and SHOULD apply the format's normal minimum/maximum block-size constraints after decoding. The current supported encoded range is `4 <= volume_block_size_kib <= 8192`, matching a decoded record-size range of 4 KiB to 8 MiB.

`frame_payload_size` is a uint32; its maximum value is implicitly bounded by `(volume_block_size_kib * 1024) - 512`.

For normal slice-channel frames, any frame with `END = 0` MUST use the full
available payload area, meaning
`frame_payload_size = (volume_block_size_kib * 1024) - 512`. A payload smaller
than the full per-record capacity is only allowed on the final frame of that
channel within the slice (`END = 1`). This rule does not require
`archive_end` to be full-sized.

### `archive_label`

A UTF-8 label, not an archive identifier. It is encoded as a 65-byte NUL-terminated and NUL-padded field, with at most 64 usable bytes before the first NUL byte. `archive_uuid` remains the authoritative archive identity.

### `signature` and `frame_hash`

The `signature` and `frame_hash` fields are defined in [docs/spec/00-format-common.md](00-format-common.md):

- `frame_hash` is a BLAKE3 digest over the canonical image of the entire frame.
- `signature` is a 72-byte field used when the `SIGNED` flag is set. Bytes 0-7 hold a 64-bit key ID; bytes 8-71 hold a raw 64-byte Ed25519 signature over `NeoTape-frame\0 || frame_hash`. The context string includes its trailing NUL byte. This mirrors OpenBSD signify's Ed25519 signature payload without the leading two `Ed` bytes.

### `sideband_data`

- `sideband_data` is a 128-byte optional area whose meaning is defined by `channel_type`. The `SIDEBAND` flag signals that the area carries meaningful data; when `SIDEBAND` is clear, writers MUST write all 128 bytes as zero and readers MUST ignore the field's contents.
- In `header_version=1`, `ch_content`, `ch_metadata`, and `archive_end` MUST NOT set `SIDEBAND` and MUST zero-fill `sideband_data`. `ch_fec` MUST set `SIDEBAND = 1` and MUST encode the FEC group descriptor defined in [docs/spec/04-fec-channel.md](04-fec-channel.md). Future `channel_type` values (4–254) may define their own sideband encoding, internal structure, and per-frame consistency rules.
- `sideband_data` is included in `frame_hash` like every other fixed header field; the canonical image only zeroes `signature` and `frame_hash`. Consequently it is integrity-protected by `frame_hash` and, when `SIGNED` is set, by the Ed25519 signature.
- Readers that do not understand the sideband encoding for a given `channel_type` MUST ignore `sideband_data` but MUST still include it in `frame_hash` verification.

## Channel Types

The `channel_type` field identifies the frame's role:

| Value | Name            | Meaning                                              |
| ----- | --------------- | ---------------------------------------------------- |
| 1     | `ch_content`  | Payload bytes belonging to the slice content stream. |
| 2     | `ch_metadata` | Advisory metadata bytes for the slice.               |
| 3     | `ch_fec`      | FEC repair-symbol bytes for protected `ch_content`.  |
| 255   | `archive_end` | Clean end-of-archive marker.                         |

Values 0 and 4–254 are reserved for future channels. Validation behavior for
unknown channels is defined in [docs/spec/05-validation.md](05-validation.md).
A future salvage mode MAY skip unknown channels only when it can do so without
breaking frame/slice sequence continuity.

## Flags

`flags` is a 64-bit field.

| Bit  | Name          | Meaning                                                                                                |
| ---- | ------------- | ------------------------------------------------------------------------------------------------------ |
| 0    | `END`       | Last frame of the current channel within the slice.                                                    |
| 1    | `SIGNED`    | `signature` contains an 8-byte key ID plus Ed25519 signature over `NeoTape-frame\0 \|\| frame_hash`. |
| 2    | `SIDEBAND`  | `sideband_data` carries meaningful, channel-type-defined data. MUST be set for `ch_fec` and clear for `ch_content`, `ch_metadata`, and `archive_end` in `header_version=1`; when clear, `sideband_data` MUST be all zero. |
| 3    | `FEC_PROTECTED` | Only valid on `ch_content`. Marks this frame as real protected source material for a following `ch_fec` group. |
| 4-62 | _reserved_  | Must be zero.                                                                                          |
| 63   | `CLEAN_END` | Only valid for`archive_end`. Must be `1` on a valid end-of-archive frame.                          |

The `archive_end` control frame sets `END = 1`, and `CLEAN_END = 1`.

`FEC_PROTECTED` semantics:

- `FEC_PROTECTED` MAY be set only on `ch_content` frames.
- `ch_metadata`, `ch_fec`, and `archive_end` MUST clear `FEC_PROTECTED`.
- When set, the frame is real protected source material in a following
  `ch_fec` group.
- When clear, the frame is not protected by `ch_fec`, and no later `ch_fec`
  frame may claim it as protected source material.

## Channel Semantics

### `ch_content` / `ch_metadata` / `ch_fec`

- `END` marks the final frame of the current channel within the slice.
- Within each slice, metadata frames precede all non-metadata frames: zero or more `ch_metadata` frames MAY appear first, followed by one or more payload-bearing runs containing `ch_content` and optionally `ch_fec`. A slice MUST contain at least one frame across its channels. Writers MUST NOT place `ch_metadata` after the first `ch_content` or `ch_fec` frame within the same slice.
- A slice MAY contain only metadata. Such a slice has one or more `ch_metadata` frames and no `ch_content` frames.
- Each slice MAY contain at most one contiguous `ch_metadata` run. `ch_content` and `ch_fec` need not be physically contiguous: after metadata, the writer MAY alternate repeated content runs and FEC runs within the same slice. The preferred layout for the defined FEC profile is repeated local `32C + 4F` runs.
- `ch_fec` frames describe protected `ch_content` ranges within the same slice. They MUST NOT appear before the first `ch_content` frame of that slice.
- A protected content run is a contiguous run of `ch_content` frames with
  `FEC_PROTECTED = 1` that is described by one immediately following `ch_fec`
  group.
- A writer MUST immediately follow each protected content run with one matching
  `ch_fec` group. No later `ch_content` frame may appear before that group's
  `ch_fec` frames.
- For each `ch_fec` group, `source_content_frame_start` MUST equal the
  `channel_frame_seq_num` of the first frame in the protected run.
- For each `ch_fec` group, `source_frame_count` MUST equal the number of real
  `ch_content` frames in the protected run.
- Every real `ch_content` frame in the protected range described by a `ch_fec`
  group MUST have `FEC_PROTECTED = 1`.
- Within one slice, a writer that starts emitting `FEC_PROTECTED = 1`
  `ch_content` frames MUST continue using protected runs for all later
  `ch_content` frames in that slice. It MUST NOT switch later content frames in
  the same slice back to `FEC_PROTECTED = 0`.
- Within one archive, if any `ch_content` frame uses `FEC_PROTECTED = 1`, a
  conforming writer MUST use the same protected-run discipline for all later
  `ch_content` frames in the archive, except that a slice may contain no
  `ch_content` at all.
- `channel_frame_seq_num` is scoped to `(slice_seq_num, channel_type)`. It starts at 0 on the first frame of that channel in the slice and increments only within that channel, even if frames of other channels appear in between. The sequence does not continue across different `channel_type` values.

### `archive_end`

- Written as the final record of a cleanly completed archive.
- `channel_type = archive_end`.
- `END = 1`, `CLEAN_END = 1`.
- `frame_payload_size` is normally `0`. A non-zero payload MAY carry optional end-of-archive metadata; its interpretation is implementation-specific.
- `slice_seq_num = 0`, `channel_frame_seq_num = 0`.
- Because `archive_end` is a control frame identified by `channel_type`, these scoped sequence fields are fixed canonical values, not membership in slice 0 or a normal channel group.

## Hierarchy

```
Archive (archive_uuid)
  └── Volume (backend-defined physical/virtual volume)
      └── Slice (slice_seq_num)
          └── Channel (channel_type: ch_metadata / ch_content / ch_fec)
              └── Frame (global_frame_seq_num, channel_frame_seq_num)
```

- **Archive** — authoritative logical backup instance, identified by `archive_uuid`.
- **Volume** — backend-defined container; not an authoritative logical record. `volume_seq_num` is advisory.
- **Slice** — unit of ordering; identified by `slice_seq_num`. May span frames and backend volumes.
- **Channel** — partitions a slice into `ch_metadata`, `ch_content`, and optional `ch_fec`. Metadata precedes all non-metadata frames.
- **Frame** — concrete transport record. Every frame has exactly one `channel_type`.

## Sequence Numbering

- `global_frame_seq_num` starts at 0, increments by 1 for every frame including `archive_end`. Does not reset at volume boundaries.
- `slice_seq_num` starts at 0, increments per slice. Same value across all frames in a slice and across volumes. `archive_end` uses the canonical control-frame value `0`.
- `channel_frame_seq_num` starts at 0 on the first frame of each `(slice_seq_num, channel_type)` stream, increments only within that channel, and resets only when the slice changes. `archive_end` uses the canonical control-frame value `0`.

## Tape Layout

```
[optional recovery bundle]
[optional filemark]
Slice 0 tape file  (ch_metadata / ch_content / optional ch_fec frames)
filemark
Slice 1 tape file
filemark
...
Archive End frame (single record)
filemark
```

Volume boundaries are physical/operator events, detected by filemark transitions. Every frame repeats `volume_block_size_kib`, `archive_uuid`, `archive_label`, and advisory `volume_seq_num`. A slice MAY span backend volumes; sequence continuity does not reset at volume boundaries.

Within a backend volume, all frames SHOULD carry the same `volume_seq_num`. `volume_seq_num` is a human/operator-facing ordinal; together with `archive_label` it forms the display label `${archive_label} #${volume_seq_num}`. `archive_uuid` remains the authoritative archive identity.

## Reader Model

1. Read one backend record/tape block, parse its fixed 512-byte header.
2. Decode `volume_block_size_kib`; validate record size when the backend exposes it.
3. Apply the relevant validation rules from [docs/spec/05-validation.md](05-validation.md).
4. Dispatch by `channel_type`: `ch_content` — emit payload bytes; `ch_metadata` — skip or parse advisory bytes; `ch_fec` — skip in normal mode or hand to a repair-capable reader; `archive_end` — verify `CLEAN_END` and finish.
5. Ignore `sideband_data` unless the `SIDEBAND` flag is set and the `channel_type` defines an interpretation; always include `sideband_data` in `frame_hash` verification.

Archive continuity rules, `archive_end` checks, and mode-specific exceptions
are centralized in [docs/spec/05-validation.md](05-validation.md).
