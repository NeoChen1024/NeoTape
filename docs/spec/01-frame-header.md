# Frame Header Layout

Status: normative.

All NeoTape records use a single unified 512-byte fixed header. The final 32 bytes are `frame_hash`, a BLAKE3 digest over the canonical image of the entire frame.

## Fixed Header Layout

| Field                          | Size (bytes) | Notes                                                                           |
| ------------------------------ | ------------ | ------------------------------------------------------------------------------- |
| `magic`                        | 8            | `NeoTape\0`                                                                     |
| `header_version`               | 1            | Currently `1`; bump on layout-breaking changes.                                 |
| `channel_type`                 | 1            | `uint8` enum.  See Channel Types below.                                         |
| `volume_block_size_kib`        | 2            | Record size for this volume, encoded in KiB.                                    |
| `archive_uuid`                 | 37           | NUL-terminated UUID string.                                                     |
| `archive_label`                | 65           | NUL-terminated/padded human-readable archive label.                             |
| `volume_seq_num`               | 8            | Advisory volume sequence number; part of the volume label/name.                 |
| `global_frame_seq_num`         | 8            | Monotonically increasing across **all** frames, including `archive_end`.        |
| `logical_slice_seq_num`        | 8            | `0` for the `archive_end` control frame.                                        |
| `frame_seq_num_within_channel` | 8            | `1` for the `archive_end` control frame.                                        |
| `frame_payload_size`           | 8            | Meaningful payload bytes after the fixed header.                                |
| `flags`                        | 8            | `uint64`.  See Flags below.                                                     |
| `_reserved`                    | 190          | Zero-filled Padding.                                                            |
| `signature`                    | 128          | Binary signify-style signature over `frame_hash`; used when `SIGNED` is set.    |
| `frame_hash`                   | 32           | BLAKE3 over the canonical image of the whole frame.                             |

Total: 512 bytes.

Datatype rules from `docs/spec/00-format-common.md` still apply (little-endian integers, NUL-terminated strings, BLAKE3 algorithm, etc.).

## Field Semantics

### `volume_block_size_kib`

Stores the NeoTape record size in KiB, not bytes. The decoded record size is `volume_block_size_kib * 1024` bytes. Writers MUST encode only whole-KiB record sizes; readers MUST reject `0` and SHOULD apply the format's normal minimum/maximum block-size constraints after decoding. The current supported encoded range is `4 <= volume_block_size_kib <= 8192`, matching a decoded record-size range of 4 KiB to 8 MiB.

`frame_payload_size` MUST be less than or equal to `(volume_block_size_kib * 1024) - 512`.

### `archive_label`

A UTF-8 label, not an archive identifier. It is encoded as a 65-byte NUL-terminated and NUL-padded field, with at most 64 usable bytes before the first NUL byte. `archive_uuid` remains the authoritative archive identity.

### `signature` and `frame_hash`

The `signature` and `frame_hash` fields are defined in [docs/spec/00-format-common.md](00-format-common.md):

- `frame_hash` is a BLAKE3 digest over the canonical image of the entire frame.
- `signature` holds a binary signify-style signature over `frame_hash` when the `SIGNED` flag is set.

## Channel Types

The `channel_type` field identifies the frame's role:

| Value | Name          | Meaning                                                      |
| ----- | ------------- | ------------------------------------------------------------ |
| 1     | `ch_content`  | Payload bytes belonging to the logical slice content stream. |
| 2     | `ch_metadata` | Advisory metadata bytes for the logical slice.               |
| 255   | `archive_end` | Clean end-of-archive marker.                                 |

Values 0, 3–254 are reserved for future channels. A reader that encounters an unknown `channel_type` MUST reject the archive in normal restore mode. A future salvage mode MAY skip unknown channels only when it can do so without breaking frame/slice sequence continuity.

## Flags

`flags` is a 64-bit field.

| Bit   | Name        | Meaning                                                                     |
| ----- | ----------- | --------------------------------------------------------------------------- |
| 0     | `START`     | First frame of the current channel group.                                   |
| 1     | `END`       | Last frame of the current channel group.                                    |
| 2     | `SIGNED`    | `signature` contains a binary signify-style signature over `frame_hash`.    |
| 3–62  | _reserved_  | Must be zero.                                                               |
| 63    | `CLEAN_END` | Only valid for `archive_end`. Must be `1` on a valid end-of-archive frame.  |

The `archive_end` control frame sets `START = 1`, `END = 1`, and `CLEAN_END = 1`.

## Channel Semantics

### `ch_content` / `ch_metadata`

- `START`/`END` describe the current channel group within the logical slice.
- Within each logical slice, metadata frames precede data frames: zero or more `ch_metadata` frames MAY appear first, followed by zero or more `ch_content` frames. A logical slice MUST contain at least one frame across its channels. Writers MUST NOT place `ch_metadata` after `ch_content` within the same logical slice.
- A logical slice MAY contain only metadata. Such a slice has one or more `ch_metadata` frames and no `ch_content` frames.
- Each logical slice MAY contain at most one contiguous `ch_metadata` group and at most one contiguous `ch_content` group.
- `frame_seq_num_within_channel` is scoped to `(logical_slice_seq_num, channel_type)`. It starts at 1 for each channel group and increments only within that channel. It does not continue across `ch_metadata` and `ch_content`.

### `archive_end`

- Written as the final record of a cleanly completed archive.
- `channel_type = archive_end`.
- `START = 1`, `END = 1`, `CLEAN_END = 1`.
- `frame_payload_size` is normally `0`. A non-zero payload MAY carry optional end-of-archive metadata; its interpretation is implementation-specific.
- `logical_slice_seq_num = 0`, `frame_seq_num_within_channel = 1`.

## Logical Hierarchy

```
Archive (archive_uuid)
  └── Volume (backend-defined physical/virtual volume)
      └── Logical Slice (logical_slice_seq_num)
          └── Channel (channel_type: ch_metadata / ch_content)
              └── Frame (global_frame_seq_num, frame_seq_num_within_channel)
```

- **Archive** — authoritative logical backup instance, identified by `archive_uuid`.
- **Volume** — backend-defined container; not an authoritative logical record. `volume_seq_num` is advisory.
- **Logical Slice** — unit of ordering; identified by `logical_slice_seq_num`. May span frames and backend volumes.
- **Channel** — partitions a slice into `ch_metadata` and `ch_content`. Metadata precedes content.
- **Frame** — concrete transport record. Every frame has exactly one `channel_type`.

## Sequence Numbering

- `global_frame_seq_num` starts at 1, increments by 1 for every frame including `archive_end`. Does not reset at volume boundaries.
- `logical_slice_seq_num` starts at 1, increments per slice. Same value across all frames in a slice and across volumes. `archive_end` uses `0`.
- `frame_seq_num_within_channel` starts at 1 per channel group, increments only within that channel. Resets on new channel group or new slice. `archive_end` uses `1`.

## Tape Layout

```
[optional recovery bundle]
[optional filemark]
Logical Slice 1 tape file  (ch_metadata / ch_content frames)
filemark
Logical Slice 2 tape file
filemark
...
Archive End frame (single record)
filemark
```

Volume boundaries are physical/operator events, detected by filemark transitions. Every frame repeats `volume_block_size_kib`, `archive_uuid`, `archive_label`, and advisory `volume_seq_num`. A logical slice MAY span backend volumes; sequence continuity does not reset at volume boundaries.

Within a backend volume, all frames SHOULD carry the same `volume_seq_num`. `volume_seq_num` is a human/operator-facing ordinal; together with `archive_label` it forms the display label `${archive_label} #${volume_seq_num}`. `archive_uuid` remains the authoritative archive identity.

## Reader Model

1. Read one backend record/tape block, parse its fixed 512-byte header.
2. Decode `volume_block_size_kib`; validate record size when the backend exposes it.
3. Validate magic, version, and `frame_hash`.
4. Dispatch by `channel_type`: `ch_content` / `ch_metadata` — stream payload and track START/END; `archive_end` — verify `CLEAN_END` and finish.

After a filemark, validate archive continuity using `archive_uuid`, `global_frame_seq_num`, `logical_slice_seq_num`, and `frame_seq_num_within_channel`. `volume_seq_num` is advisory for operator prompts and diagnostics only.
