# Frames, Slices, and Channels

Status: normative.

## Frame Model

Each Frame occupies exactly one NeoTape record (`volume_block_size_kib * 1024` bytes):

```
  +-- 512-byte Frame Header --+-- frame_payload_size payload bytes --+-- padding --+
  +---------------------------+--------------------------------------+-------------+
  <------------------------ volume_block_size_kib * 1024 -------------------------->
```

`frame_payload_size` MUST be less than or equal to `(volume_block_size_kib * 1024) - 512`. A Frame MUST NOT span multiple NeoTape records and MUST NOT span archive volumes. A partially written Frame is not part of the archive.

## Logical Slices and Channels

A logical slice is a writer-declared logical content grouping, identified by `logical_slice_seq_num`. A logical slice consists of one or more frames partitioned into channels:

```
LogicalSlice[k] =
    (optional) ch_metadata.group[k] + ch_content.group[k]
```

Where each group is zero or more frames in the same channel. At least one frame must be present across all channels.

Each logical slice MAY contain at most one contiguous `ch_metadata` group and at most one contiguous `ch_content` group. Metadata, when present, precedes content. A logical slice MAY contain only metadata (a metadata-only slice).

A logical slice MAY span multiple archive volumes. Sequence continuity is maintained across volume boundaries: `global_frame_seq_num`, `logical_slice_seq_num`, and `frame_seq_num_within_channel` do not reset at a volume boundary.

## Channel Types

The `channel_type` field identifies the frame's channel:

| Value | Name            | Description                                                  |
| ----- | --------------- | ------------------------------------------------------------ |
| 1     | `ch_content`  | Payload bytes belonging to the logical slice content stream. |
| 2     | `ch_metadata` | Advisory metadata bytes for the logical slice.               |
| 255   | `archive_end` | Clean end-of-archive marker.                                 |

Values 0, 3–254 are reserved for future channels. A reader that encounters an unknown `channel_type` MUST reject the archive in normal restore mode.

A normal payload reader (e.g. `neotape restore`) MUST emit only `ch_content` frame payload bytes. It MUST NOT emit `ch_metadata` bytes to stdout.

`ch_metadata` frames are advisory in normal restore mode:

- A payload reader or restore-mode server MUST NOT reject a slice or archive
  solely because a frame already identified as `ch_metadata` is missing or
  unusable, as long as record framing, archive identity, and sequence
  continuity remain unambiguous.
- If a `ch_metadata` frame fails `frame_hash` verification but its header and
  surrounding sequence remain parseable enough to preserve archive identity
  and ordering, the reader SHOULD log a warning and continue.

## START and END Flags

- `START` — first frame of a channel group within the logical slice.
- `END` — final frame of a channel group within the logical slice.

Both flags apply to the current `channel_type` group:

| START | END | Meaning                                   |
| ----- | --- | ----------------------------------------- |
| 0     | 0   | Continuation frame.                       |
| 0     | 1   | Last frame of this channel group.         |
| 1     | 0   | First frame of this channel group.        |
| 1     | 1   | Single-frame group (both first and last). |

The `archive_end` frame sets `START = 1`, `END = 1`, and `CLEAN_END = 1`.

## Per-Frame Integrity

Each frame is individually integrity-checked by `frame_hash`, a BLAKE3 digest over the canonical image of the entire frame (header, payload, and padding). There is no separate slice-level hash. See [docs/spec/00-format-common.md](00-format-common.md) for the hash calculation rules.

## Frame Sequence Numbering

- `global_frame_seq_num` — starts at 1 and increments by 1 for every frame in the archive, including `archive_end`. Does not reset at volume boundaries.
- `logical_slice_seq_num` — starts at 1 for the first logical slice, increments by 1 for each new slice. All frames in the same slice carry the same value, even across volumes. `archive_end` uses `0`.
- `frame_seq_num_within_channel` — scoped to `(logical_slice_seq_num, channel_type)`. Starts at 1 for each channel group, increments only within that channel. Resets when a new channel group or new logical slice starts. `archive_end` uses `1`.

All three are `uint64`. The reader validates that sequence numbers are contiguous within their scope.

## Metadata Channel Ordering

Within each logical slice, metadata frames MUST precede content frames. Writers MUST NOT place `ch_metadata` after `ch_content` within the same logical slice. `frame_seq_num_within_channel` is scoped per-channel and does not continue across `ch_metadata` and `ch_content`.

## Logical Slice Completion

The writer decides when to close a logical slice. When it closes:

1. The current frame becomes the final frame for its channel group and carries the `END` flag.
2. The writer MAY follow with `ch_metadata` frames only if the current group was metadata (i.e., no `ch_content` frames were written yet).
3. After all frames are committed, the writer writes a filemark to close the slice tape file.

## Archive End Frame

- Written as the final record of a cleanly completed archive.
- `channel_type = archive_end`.
- `START = 1`, `END = 1`, `CLEAN_END = 1`.
- `frame_payload_size` is normally `0`. A non-zero payload MAY carry optional end-of-archive metadata; its interpretation is implementation-specific.
- `logical_slice_seq_num = 0`, `frame_seq_num_within_channel = 1`.
