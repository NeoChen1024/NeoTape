# Frame Header Layout

Status: normative.

This chapter is the authoritative source for the exact on-wire Frame Header
layout, field widths, field order, and channel/flag assignments. Other
chapters may reference header fields conceptually, but they MUST defer to this
chapter for exact sizes and positions.

All NeoTape records use a single unified 512-byte fixed header. The final 32 bytes are `frame_hash`, a BLAKE3 digest over the canonical image of the entire frame.

## Fixed Fields

| Field                            | datatype       | size (in bytes) | Requirement | Notes                                                                                                       |
| -------------------------------- | -------------- | --------------- | ----------- | ----------------------------------------------------------------------------------------------------------- |
| `magic`                        | `char[8]`    | 8               | MUST        | Fixed NeoTape identifier:`NeoTape\0`.                                                                     |
| `header_version`               | `uint8`      | 1               | MUST        | Currently `1`; bump on layout-breaking changes.                                                           |
| `channel_type`                 | `uint8_enum` | 1               | MUST        | Frame role. See [Channel Types](#channel-types).                                                             |
| `volume_block_size_kib`        | `uint16`     | 2               | MUST        | Record size for this volume, encoded in KiB.                                                                |
| `archive_uuid`                 | `nt_uuid`    | 37              | MUST        | NUL-terminated UUID string.                                                                                 |
| `archive_label`                | `nt_name`    | 65              | SHOULD      | NUL-terminated/padded human-readable archive label.                                                         |
| `volume_seq_num`               | `uint64`     | 8               | SHOULD      | Advisory volume sequence number; part of the volume label/name.                                             |
| `global_frame_seq_num`         | `uint64`     | 8               | MUST        | Monotonically increasing across **all** frames, including `archive_end`.                              |
| `slice_seq_num`                | `uint64`     | 8               | MUST        | `0` for the `archive_end` control frame.                                                                |
| `channel_frame_seq_num`        | `uint64`     | 8               | MUST        | `0` for the `archive_end` control frame.                                                                |
| `frame_payload_size`           | `uint32`     | 4               | MUST        | Meaningful payload bytes after the fixed header.                                                            |
| `flags`                        | `uint64`     | 8               | MUST        | Frame flags. See [Flags](#flags).                                                                          |
| `_reserved`                    | `byte[250]`  | 250             | MUST        | Zero-filled padding.                                                                                        |
| `signature`                    | `byte[72]`   | 72              | MAY         | 8-byte key ID plus 64-byte Ed25519 signature over `NeoTape-frame\0 \|\| frame_hash` when `SIGNED` is set. |
| `frame_hash`                   | `nt_hash`    | 32              | MUST        | BLAKE3 over the canonical image of the whole frame.                                                         |

Total: 512 bytes.

Datatype rules from `docs/spec/00-format-common.md` still apply (little-endian integers, NUL-terminated strings, BLAKE3 algorithm, etc.).

## Field Semantics

### `volume_block_size_kib`

Stores the NeoTape record size in KiB, not bytes. The decoded record size is `volume_block_size_kib * 1024` bytes. Writers MUST encode only whole-KiB record sizes; readers MUST reject `0` and SHOULD apply the format's normal minimum/maximum block-size constraints after decoding. The current supported encoded range is `4 <= volume_block_size_kib <= 8192`, matching a decoded record-size range of 4 KiB to 8 MiB.

`frame_payload_size` is a uint32; its maximum value is implicitly bounded by `(volume_block_size_kib * 1024) - 512`.

### `archive_label`

A UTF-8 label, not an archive identifier. It is encoded as a 65-byte NUL-terminated and NUL-padded field, with at most 64 usable bytes before the first NUL byte. `archive_uuid` remains the authoritative archive identity.

### `signature` and `frame_hash`

The `signature` and `frame_hash` fields are defined in [docs/spec/00-format-common.md](00-format-common.md):

- `frame_hash` is a BLAKE3 digest over the canonical image of the entire frame.
- `signature` is a 72-byte field used when the `SIGNED` flag is set. Bytes 0-7 hold a 64-bit key ID; bytes 8-71 hold a raw 64-byte Ed25519 signature over `NeoTape-frame\0 || frame_hash`. The context string includes its trailing NUL byte. This mirrors OpenBSD signify's Ed25519 signature payload without the leading two `Ed` bytes.

## Channel Types

The `channel_type` field identifies the frame's role:

| Value | Name            | Meaning                                                      |
| ----- | --------------- | ------------------------------------------------------------ |
| 1     | `ch_content`  | Payload bytes belonging to the slice content stream.         |
| 2     | `ch_metadata` | Advisory metadata bytes for the slice.                       |
| 255   | `archive_end` | Clean end-of-archive marker.                                 |

Values 0, 3–254 are reserved for future channels. A reader that encounters an unknown `channel_type` MUST reject the archive in normal restore mode. A future salvage mode MAY skip unknown channels only when it can do so without breaking frame/slice sequence continuity.

## Flags

`flags` is a 64-bit field.

| Bit  | Name          | Meaning                                                                                                |
| ---- | ------------- | ------------------------------------------------------------------------------------------------------ |
| 0    | `END`       | Last frame of the current channel group.                                                               |
| 1    | `SIGNED`    | `signature` contains an 8-byte key ID plus Ed25519 signature over `NeoTape-frame\0 \|\| frame_hash`. |
| 2-62 | _reserved_  | Must be zero.                                                                                          |
| 63   | `CLEAN_END` | Only valid for `archive_end`. Must be `1` on a valid end-of-archive frame.                         |

The `archive_end` control frame sets `END = 1`, and `CLEAN_END = 1`.

## Channel Semantics

### `ch_content` / `ch_metadata`

- `END` describes the current channel group within the slice.
- Within each slice, metadata frames precede data frames: zero or more `ch_metadata` frames MAY appear first, followed by zero or more `ch_content` frames. A slice MUST contain at least one frame across its channels. Writers MUST NOT place `ch_metadata` after `ch_content` within the same slice.
- A slice MAY contain only metadata. Such a slice has one or more `ch_metadata` frames and no `ch_content` frames.
- Each slice MAY contain at most one contiguous `ch_metadata` group and at most one contiguous `ch_content` group.
- `channel_frame_seq_num` is scoped to `(slice_seq_num, channel_type)`. It starts at 0 for each channel group and increments only within that channel. A frame with `channel_frame_seq_num = 0` is the first frame of its channel group. The sequence does not continue across `ch_metadata` and `ch_content`.

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
          └── Channel (channel_type: ch_metadata / ch_content)
              └── Frame (global_frame_seq_num, channel_frame_seq_num)
```

- **Archive** — authoritative logical backup instance, identified by `archive_uuid`.
- **Volume** — backend-defined container; not an authoritative logical record. `volume_seq_num` is advisory.
- **Slice** — unit of ordering; identified by `slice_seq_num`. May span frames and backend volumes.
- **Channel** — partitions a slice into `ch_metadata` and `ch_content`. Metadata precedes content.
- **Frame** — concrete transport record. Every frame has exactly one `channel_type`.

## Sequence Numbering

- `global_frame_seq_num` starts at 0, increments by 1 for every frame including `archive_end`. Does not reset at volume boundaries.
- `slice_seq_num` starts at 0, increments per slice. Same value across all frames in a slice and across volumes. `archive_end` uses the canonical control-frame value `0`.
- `channel_frame_seq_num` starts at 0 per channel group, increments only within that channel. Resets on a new channel group or new slice. `archive_end` uses the canonical control-frame value `0`.

## Tape Layout

```
[optional recovery bundle]
[optional filemark]
Slice 0 tape file  (ch_metadata / ch_content frames)
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
3. Validate magic, version, and `frame_hash`.
4. Dispatch by `channel_type`: `ch_content` / `ch_metadata` — stream payload and track channel-group boundaries via `channel_frame_seq_num` and `END`; `archive_end` — verify `CLEAN_END` and finish.

After a filemark, validate archive continuity using `archive_uuid`, `global_frame_seq_num`, `slice_seq_num`, and `channel_frame_seq_num`. `volume_seq_num` is advisory for operator prompts and diagnostics only.
