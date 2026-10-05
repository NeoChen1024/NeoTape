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
| `magic`                  | `char[8]`    | 8               | MUST        | Fixed NeoTape identifier: `NeoTape\0`.                                                                    |
| `header_version`         | `uint8`      | 1               | MUST        | Currently `1`; bump on layout-breaking changes.                                                           |
| `channel_type`           | `uint8_enum` | 1               | MUST        | Frame role. See [Channel Types](#channel-types).                                                             |
| `volume_block_size_kib`  | `uint16`     | 2               | MUST        | Record size for the entire archive, encoded in KiB.                                                               |
| `archive_uuid`           | `nt_uuid`    | 37              | MUST        | NUL-terminated UUID string.                                                                                |
| `archive_label`          | `nt_name`    | 65              | SHOULD      | NUL-terminated/padded human-readable archive label.                                                        |
| `volume_seq_num`         | `uint64`     | 8               | SHOULD      | Advisory volume sequence number; part of the volume label/name.                                            |
| `global_frame_seq_num`   | `uint64`     | 8               | MUST        | Monotonically increasing across **all** frames, including `archive_end`.                            |
| `slice_seq_num`          | `uint64`     | 8               | MUST        | `0` for the `archive_end` control frame.                                                               |
|  `channel_frame_seq_num` | `uint64`     | 8               | MUST        | `0` for the `archive_end` control frame.                                                               |
| `frame_payload_size`     | `uint32`     | 4               | MUST        | Meaningful payload bytes after the fixed header.                                                           |
| `flags`                  | `uint64`     | 8               | MUST        | Frame flags. See [Flags](#flags).                                                                            |
| `_reserved`              | `byte[122]`  | 122             | MUST        | Zero-filled padding. Immediately followed by `sideband_data` at offset 280.                               |
| `sideband_data`          | `byte[128]`  | 128             | MAY         | Optional sideband data. Interpretation is defined by `channel_type`. See [Sideband Data](#sideband_data). |
| `signature`              | `byte[72]`   | 72              | MAY         | 8-byte key ID plus 64-byte Ed25519 signature over `NeoTape-frame\0 \|\| frame_hash` when `SIGNED` is set. |
| `frame_hash`             | `nt_hash`    | 32              | MUST        | BLAKE3 over the canonical image of the whole frame.                                                        |

Total: 512 bytes.

Datatype rules from `docs/spec/00-format-common.md` still apply (little-endian integers, NUL-terminated strings, BLAKE3 algorithm, etc.).

## Field Semantics

### `volume_block_size_kib`

Stores the NeoTape record size in KiB, not bytes. The decoded record size is
`volume_block_size_kib * 1024` bytes. It MUST remain constant across every frame
of an archive, including volume transitions and `archive_end`. Writers MUST
encode whole-KiB sizes in `4 <= volume_block_size_kib <= 8192`; readers MUST
reject values outside that range (4 KiB through 8 MiB decoded).

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

Both fields are defined in [00-format-common.md](00-format-common.md):
`frame_hash` under [Frame Hash Calculation](00-format-common.md#frame-hash-calculation)
and `signature` under [Signing Sequence](00-format-common.md#signing-sequence).

### `sideband_data`

- `sideband_data` is a 128-byte optional area whose meaning is defined by `channel_type`. The `SIDEBAND` flag signals that the area carries meaningful data; when `SIDEBAND` is clear, writers MUST write all 128 bytes as zero and readers MUST verify that they are zero, without interpreting them as sideband data.
- In `header_version=1`, no channel defines sideband data: every frame MUST clear `SIDEBAND` and MUST zero-fill `sideband_data`. Future `channel_type` values may define their own sideband encoding, internal structure, and per-frame consistency rules.
- `sideband_data` is included in `frame_hash` like every other fixed header field; the canonical image only zeroes `signature` and `frame_hash`. Consequently it is integrity-protected by `frame_hash` and, when `SIGNED` is set, by the Ed25519 signature.
- Unsupported channels MUST NOT be accepted as understood data; salvage readers may skip their independently framed records. All sideband bytes are included in `frame_hash` verification.

## Channel Types

The `channel_type` field identifies the frame's role:

| Value | Name            | Meaning                                              |
| ----- | --------------- | ---------------------------------------------------- |
| 1     | `ch_content`  | Payload bytes belonging to the slice content stream. |
| 2     | `ch_metadata` | Advisory metadata bytes carried in the slice.        |
| 255   | `archive_end` | Clean end-of-archive marker.                         |

Values 0 and 3–254 are reserved for future channels. Value 3 was used by the
retired `ch_fec` repair channel and is rejected like any other reserved value.
Validation behavior for unknown channels is defined in
[docs/spec/04-validation.md](04-validation.md).
Salvage mode MAY skip independently framed records of unknown channels; it
MUST NOT emit their payloads or claim complete sequence validation.

## Flags

`flags` is a 64-bit field.

| Bit  | Name          | Meaning                                                                                                |
| ---- | ------------- | ------------------------------------------------------------------------------------------------------ |
| 0    | `END`       | Last frame of the current channel within the slice.                                                    |
| 1    | `SIGNED`    | `signature` contains an 8-byte key ID plus Ed25519 signature over `NeoTape-frame\0 \|\| frame_hash`. |
| 2    | `SIDEBAND`  | `sideband_data` carries meaningful, channel-type-defined data. No `header_version=1` channel defines sideband data, so this bit MUST be clear; when clear, `sideband_data` MUST be all zero. |
| 3-62 | _reserved_  | Must be zero. Bit 3 was the retired `FEC_PROTECTED` flag.                                              |
| 63   | `CLEAN_END` | Only valid for `archive_end`. Must be `1` on a valid end-of-archive frame.                          |

The `archive_end` control frame sets `END = 1`, and `CLEAN_END = 1`.

## Related Chapters

- Slice and channel structure, sequence numbering, and the `archive_end`
  frame's field values: [03-frames-and-slices.md](03-frames-and-slices.md).
- Logical hierarchy and tape layout: [05-volume-layout.md](05-volume-layout.md).

## Reader Model

1. Read one backend record/tape block, parse its fixed 512-byte header.
2. Decode `volume_block_size_kib`; validate record size when the backend exposes it.
3. Apply the relevant validation rules from [docs/spec/04-validation.md](04-validation.md).
4. Dispatch by `channel_type`: `ch_content` — emit payload bytes; `ch_metadata` — skip or parse advisory bytes; `archive_end` — verify `CLEAN_END` and finish.
5. Verify that `SIDEBAND` is clear and `sideband_data` is all zero; always include it in `frame_hash` verification.

Archive continuity rules, `archive_end` checks, and mode-specific exceptions
are centralized in [docs/spec/04-validation.md](04-validation.md).
