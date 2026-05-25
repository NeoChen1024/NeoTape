# Frames and Slices

Status: extracted from RFC_Draft.md §§7, 11; normative.

## Frame Model

A logical slice consists of one or more Frames:

```
LogicalSlice[k] = Frame[k,1].payload + Frame[k,2].payload + ... + Frame[k,M].payload
```

Each Frame occupies exactly one NeoTape record (`volume_block_size` bytes):

```
  +-- 1024-byte Frame Header --+-- frame_payload_size payload bytes --+-- padding --+
  +----------------------------+--------------------------------------+------------+
  <-------------------------- volume_block_size ------------------------------------>
```

`frame_payload_size` MUST be strictly less than or equal to `volume_block_size - 1024`. A Frame MUST NOT span multiple NeoTape records and MUST NOT span archive volumes. A partially written Frame is not part of the archive.

A logical slice MAY span multiple Frames and MAY span multiple archive volumes. When a volume boundary occurs mid-slice, the next volume continues the same `logical_slice_seq_num` with the next Frame sequence number. No partial-Frame continuation is used.

## Frame Content Types

Each Frame has a `frame_content_type`:

| Content type    | Description                                                       |
|-----------------|-------------------------------------------------------------------|
| `SLICE_CONTENT` | Opaque payload bytes belonging to the logical slice byte stream.  |
| `SLICE_METADATA` | Advisory metadata bytes associated with a logical slice.         |

A normal payload reader (e.g. `neotape restore`) MUST emit only `SLICE_CONTENT` Frame payload bytes. It MUST NOT emit `SLICE_METADATA` bytes to stdout.

## START and END Flags

- `START` — first Frame of a content-type group within the logical slice.
- `END` — final Frame of a content-type group within the logical slice.

Both flags apply to the current `frame_content_type` group:

| START | END | Meaning |
|-------|-----|---------|
| 0     | 0   | Continuation frame. |
| 0     | 1   | Last frame of this content-type group. |
| 1     | 0   | First frame of this content-type group. |
| 1     | 1   | Single-frame group (both first and last). |

The END Frame Header carries `slice_content_size` and `slice_content_blake3` for the current `frame_content_type` group. For `SLICE_CONTENT`, these describe the concatenated content payload. For `SLICE_METADATA`, they describe the concatenated metadata payload.

## Slice-Level Integrity

### SLICE_CONTENT Hash

`slice_content_blake3` is computed over exactly `slice_content_size` bytes of concatenated payload from all `SLICE_CONTENT` Frames in the logical slice, in Frame sequence order. `SLICE_METADATA` Frame bytes are NOT included.

### SLICE_METADATA Hash

`slice_content_blake3` is computed over exactly `slice_content_size` bytes of concatenated metadata from all `SLICE_METADATA` Frames in the metadata group, in Frame sequence order. `SLICE_CONTENT` Frame bytes are NOT included.

### Common Rules

- Non-END Frames MUST set both `slice_content_size` and `slice_content_blake3` to zero.
- Frame-level hashes (`frame_payload_blake3`) are independent. A reader MUST compute each slice-level BLAKE3 directly from the relevant concatenated Frame payload bytes, not by combining per-frame hashes.

## Logical Slice Completion

The writer decides when to close a logical slice. It does not need to know the final `slice_content_size` when the slice begins. When it decides to close:

1. The current Frame becomes the final `SLICE_CONTENT` Frame and carries the `END` flag.
2. The Frame Header records `slice_content_size` and `slice_content_blake3`.
3. The writer MAY follow with zero or more `SLICE_METADATA` Frames (advisory).
4. After all Frames are committed, the writer writes a filemark to close the slice tape file.

The actual `slice_content_size` is known only at this point and is recorded in the final Frame Header.

## SLICE_METADATA Frames

`SLICE_METADATA` Frames are advisory:
- A reader MUST NOT reject a slice or archive solely because of missing, truncated, or corrupt `SLICE_METADATA` Frames.
- If `frame_payload_blake3` verification fails for a `SLICE_METADATA` Frame, the reader SHOULD log a warning and continue.
- `SLICE_METADATA` Frame bytes are in restricted ar format (see `00-format-common.md`).

If EOT occurs before all `SLICE_METADATA` Frames can be committed, the next volume SHOULD resume with a Volume Header followed by the next complete `SLICE_METADATA` Frame. No partial Frame continuation.

## Frame Sequence Numbering

- `logical_slice_seq_num` — incremented per logical slice within the archive.
- `global_frame_seq_num` — monotonically increasing across all Frames in the archive.
- `frame_seq_num_within_slice` — incremented per Frame within the current logical slice.

All three are `uint64`. The reader validates that Frame sequence numbers within a slice tape file are contiguous and start from 1.

## Multi-Channel Frame Content Types

Future spec versions may allocate additional `frame_content_type` values. Arbitrary mixing of content types within a slice tape file is allowed: `frame_seq_num_within_slice` provides unambiguous ordering, so content-type groups need not be contiguous.

A reader that does not recognize a new `frame_content_type` should fail at volume-open time, since an unrecognized content type implies a format version it was not built for.
