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
```

At least one frame must be present across all channels. Each slice MAY contain
at most one contiguous `ch_metadata` run. Metadata, when present, precedes all
non-metadata frames. A metadata-only slice MUST NOT contain `ch_content`; a
payload slice MUST contain at least one `ch_content` frame.

A slice MAY span multiple archive volumes. Sequence continuity is maintained across volume boundaries: `global_frame_seq_num`, `slice_seq_num`, and `channel_frame_seq_num` do not reset at a volume boundary.

## Channel Types

The `channel_type` field identifies the frame's channel:

| Value | Name            | Description                                                  |
| ----- | --------------- | ------------------------------------------------------------ |
| 1     | `ch_content`  | Payload bytes belonging to the slice content stream.         |
| 2     | `ch_metadata` | Advisory metadata bytes for the slice.                       |
| 255   | `archive_end` | Clean end-of-archive marker.                                 |

Values 0 and 3–254 are reserved for future channels. Validation behavior for
unknown channels is defined in [docs/spec/04-validation.md](04-validation.md).

A normal payload reader (e.g. `neotape restore`) MUST emit only `ch_content` frame payload bytes. It MUST NOT emit `ch_metadata` bytes to stdout.

`ch_metadata` frames are advisory in normal restore mode. The restore-mode
exception for already-identified `ch_metadata` validation failures is defined
in [docs/spec/04-validation.md](04-validation.md).

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

## Per-Frame Integrity

Each frame is individually integrity-checked by `frame_hash`, a BLAKE3 digest over the canonical image of the entire frame (header, payload, and padding). There is no separate slice-level hash. See [docs/spec/00-format-common.md](00-format-common.md) for the hash calculation rules.

## Frame Sequence Numbering

Channel ordering, completion, and sequence rules describe logical frames.
Physical retries do not create new logical frames; readers apply replay
comparison and suppression under [04-validation.md](04-validation.md#replayed-records).

- `global_frame_seq_num` — starts at 0 and increments by 1 for every frame in the archive, including `archive_end`. Does not reset at volume boundaries.
- `slice_seq_num` — starts at 0 for the first slice, increments by 1 for each new slice. All frames in the same slice carry the same value, even across volumes. `archive_end` uses the canonical control-frame value `0`.
- `channel_frame_seq_num` — scoped to `(slice_seq_num, channel_type)`. Starts at 0 on the first frame of that channel in the slice, increments only within that channel, and does not reset merely because a different channel appears later in the same slice. `archive_end` uses the canonical control-frame value `0`.

All three are `uint64`. Writers assign contiguous logical sequence numbers
within each scope. Readers apply the verified replay and unavailable-record
recovery rules in [04-validation.md](04-validation.md); physical replays do
not introduce new logical frames.
The authoritative continuity rules are defined in
[docs/spec/04-validation.md](04-validation.md).

## Metadata Channel Ordering

Within each slice, metadata frames MUST precede all non-metadata frames. Writers MUST NOT place `ch_metadata` after `ch_content` within the same slice. `channel_frame_seq_num` is scoped per-channel and does not continue across different `channel_type` values.

## Slice Completion

The writer decides when to close a slice. When it closes:

1. The final frame of every channel present in the slice carries `END`. A
   leading metadata run reaches `END` before the first content frame.
2. The writer MUST NOT emit another frame for a channel after that channel has
   reached `END`.
3. The writer MUST NOT follow with `ch_metadata` frames once any `ch_content`
   frame has been written in that slice.
4. After all frames are committed, the writer writes a filemark to close the
   slice tape file.

## Archive End Frame

- Written as the final record of a cleanly completed archive.
- `channel_type = archive_end`.
- `END = 1`, `CLEAN_END = 1`.
- `frame_payload_size` is normally `0`. A non-zero payload MAY carry optional end-of-archive metadata; its interpretation is implementation-specific.
- `slice_seq_num = 0`, `channel_frame_seq_num = 0`.
- Because `archive_end` is a control frame identified by `channel_type`, these scoped sequence fields are fixed canonical values, not membership in slice 0 or a normal channel group.
