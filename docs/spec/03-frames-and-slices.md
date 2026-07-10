# Frames, Slices, and Channels

Status: normative.

## Frame Model

Each Frame occupies exactly one NeoTape record (`volume_block_size_kib * 1024` bytes):

```
  +-- 512-byte Frame Header --+-- frame_payload_size payload bytes --+-- padding --+
  +---------------------------+--------------------------------------+-------------+
  <------------------------ volume_block_size_kib * 1024 -------------------------->
```

`frame_payload_size` MUST be less than or equal to `(volume_block_size_kib * 1024) - 512`. For normal slice-channel frames, any frame that does not carry `END = 1` MUST fill the entire payload area of its record, so only the final frame of that channel within the slice may be short. A Frame MUST NOT span multiple NeoTape records and MUST NOT span archive volumes. A partially written Frame is not part of the archive.

## Slices and Channels

A slice is a writer-declared content grouping, identified by `slice_seq_num`.
A slice consists of one or more frames in one of two forms:

```text
slice = metadata_only_slice | payload_slice

metadata_only_slice =
    one or more ch_metadata frames

payload_slice =
    [ one or more leading ch_metadata frames ]
    + one or more ch_content frames
    + optional ch_fec groups immediately following protected content runs
```

At least one frame must be present across all channels. Each slice MAY contain
at most one contiguous `ch_metadata` run. Metadata, when present, precedes all
non-metadata frames. A metadata-only slice MUST NOT contain `ch_content` or
`ch_fec`; a payload slice MUST contain at least one `ch_content` frame.

After the optional leading metadata run, the writer emits one or more `ch_content` frames. It MAY then emit one or more `ch_fec` frames describing a protected contiguous range of the immediately preceding `ch_content` stream, and later resume `ch_content` again within the same slice. This relaxed grammar allows repeated local FEC runs such as `32C + 4F`, `32C + 4F`, `32C + 4F` within a single slice.

A slice MAY span multiple archive volumes. Sequence continuity is maintained across volume boundaries: `global_frame_seq_num`, `slice_seq_num`, and `channel_frame_seq_num` do not reset at a volume boundary.

## Channel Types

The `channel_type` field identifies the frame's channel:

| Value | Name            | Description                                                  |
| ----- | --------------- | ------------------------------------------------------------ |
| 1     | `ch_content`  | Payload bytes belonging to the slice content stream.         |
| 2     | `ch_metadata` | Advisory metadata bytes for the slice.                       |
| 3     | `ch_fec`      | FEC repair symbols for protected `ch_content` ranges.        |
| 255   | `archive_end` | Clean end-of-archive marker.                                 |

Values 0 and 4–254 are reserved for future channels. Validation behavior for
unknown channels is defined in [docs/spec/05-validation.md](05-validation.md).

A normal payload reader (e.g. `neotape restore`) MUST emit only `ch_content` frame payload bytes. It MUST NOT emit `ch_metadata` or `ch_fec` bytes to stdout.

`ch_metadata` frames are advisory in normal restore mode. The restore-mode
exception for already-identified `ch_metadata` validation failures is defined
in [docs/spec/05-validation.md](05-validation.md).

`ch_fec` frames are also advisory in normal restore mode:

- A normal payload reader MAY verify and skip `ch_fec` frames, but it MUST NOT
  emit their payload bytes.
- A repair-capable reader MAY buffer `ch_content` plus `ch_fec` for one FEC
  group and reconstruct missing or damaged content before emission, subject to
  the FEC verification rules in [docs/spec/05-validation.md](05-validation.md).

## Channel Group Boundaries

- `channel_frame_seq_num = 0` — first frame of that channel within the slice.
- `END` — final frame of that channel within the slice.

These rules apply to the current `channel_type` stream within the slice:

| `channel_frame_seq_num` | END | Meaning                                     |
| ----------------------- | --- | ------------------------------------------- |
| `0`                     | `0` | First frame of a multi-frame channel stream. |
| `0`                     | `1` | Single-frame channel stream.                 |
| `>0`                    | `0` | Continuation frame.                         |
| `>0`                    | `1` | Final frame of a multi-frame channel stream. |

The `archive_end` frame sets `END = 1`, `CLEAN_END = 1`, and `channel_frame_seq_num = 0`.

For the local `32C + 4F` layout, `END` does not mark the end of each FEC
group. It marks the final `ch_fec` frame of the slice. Likewise, the final
`ch_content` frame of the slice carries `END` for `ch_content`, even if
earlier content runs were already followed by FEC frames.

## Per-Frame Integrity

Each frame is individually integrity-checked by `frame_hash`, a BLAKE3 digest over the canonical image of the entire frame (header, payload, and padding). There is no separate slice-level hash. See [docs/spec/00-format-common.md](00-format-common.md) for the hash calculation rules.

## Frame Sequence Numbering

- `global_frame_seq_num` — starts at 0 and increments by 1 for every frame in the archive, including `archive_end`. Does not reset at volume boundaries.
- `slice_seq_num` — starts at 0 for the first slice, increments by 1 for each new slice. All frames in the same slice carry the same value, even across volumes. `archive_end` uses the canonical control-frame value `0`.
- `channel_frame_seq_num` — scoped to `(slice_seq_num, channel_type)`. Starts at 0 on the first frame of that channel in the slice, increments only within that channel, and does not reset merely because a different channel appears later in the same slice. `archive_end` uses the canonical control-frame value `0`.

All three are `uint64`. The reader validates that sequence numbers are contiguous within their scope.
The authoritative continuity rules are defined in
[docs/spec/05-validation.md](05-validation.md).

## Metadata Channel Ordering

Within each slice, metadata frames MUST precede all non-metadata frames. Writers MUST NOT place `ch_metadata` after `ch_content` or `ch_fec` within the same slice. `ch_fec` frames MUST describe a protected contiguous range of prior `ch_content` from the same slice and MUST NOT appear before the first `ch_content` frame. `channel_frame_seq_num` is scoped per-channel and does not continue across different `channel_type` values.

## Slice Completion

The writer decides when to close a slice. When it closes:

1. The final frame of every channel present in the slice carries `END`. With
   interleaved channels, a channel's final frame may occur before the final
   physical frame of the slice.
2. The writer MUST NOT emit another frame for a channel after that channel has
   reached `END`.
3. The writer MUST NOT follow with `ch_metadata` frames once any `ch_content`
   or `ch_fec` frame has been written in that slice.
4. After all frames are committed, the writer writes a filemark to close the
   slice tape file.

## Archive End Frame

- Written as the final record of a cleanly completed archive.
- `channel_type = archive_end`.
- `END = 1`, `CLEAN_END = 1`.
- `frame_payload_size` is normally `0`. A non-zero payload MAY carry optional end-of-archive metadata; its interpretation is implementation-specific.
- `slice_seq_num = 0`, `channel_frame_seq_num = 0`.
- Because `archive_end` is a control frame identified by `channel_type`, these scoped sequence fields are fixed canonical values, not membership in slice 0 or a normal channel group.
