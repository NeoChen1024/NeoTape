# Frame Header

Status: field sizes and datatypes are concrete.

A NeoTape Frame is the self-describing transport record used to carry slice
content bytes or slice metadata bytes inside an archive volume. Each Frame
occupies exactly one NeoTape record of `volume_block_size` bytes. The first
1024 bytes of that record are the Frame Header; the remaining bytes contain
frame payload followed by zero padding.

The Frame Header tells a reader how many bytes in the current record are
meaningful, how those bytes contribute to the current logical slice, and whether
the bytes belong to slice content or advisory slice metadata.

The field inventory below defines the current proposed datatypes and encoded
field sizes. Exact byte offsets, enum numeric assignments, and future reserved
field allocation remain open until the byte layout is frozen.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-format-common.md](00-format-common.md).

## Placement

Every Frame Header MUST begin at byte 0 of a NeoTape record.

Every Frame occupies exactly one NeoTape record:

```text
+----------------------+----------------------+----------------------+
| 1024-byte Frame      | frame_payload_size   | zero padding to      |
| Header               | bytes of payload     | volume_block_size    |
+----------------------+----------------------+----------------------+
```

`frame_payload_size` MUST be less than or equal to:

```text
volume_block_size - 1024
```

A Frame MUST NOT span multiple NeoTape records. A Frame MUST NOT span archive
volumes. A partially written Frame is not part of the archive.

A logical slice MAY span multiple Frames and MAY span multiple archive volumes.
When a volume boundary occurs in the middle of a logical slice, the next volume
continues the same `logical_slice_seq_num` with the next Frame sequence number.
No partial-Frame continuation field is used.

The Frame Header is an archive-time commit record and MUST fit within one tape
record.

## Fixed Fields

The fixed fields should be enough to identify the archive instance, identify the
logical slice, sequence the Frame, describe the meaningful bytes in the current
record, and validate the header.

| Field                          | datatype       | size (in bytes) | Requirement | Notes                                                                                                   |
| ------------------------------ | -------------- | --------------- | ----------- | ------------------------------------------------------------------------------------------------------- |
| `magic`                      | `char[8]`    | 8               | MUST        | Fixed NeoTape identifier:`NeoTape\0`.                                                                 |
| `header_version`             | `uint8`      | 1               | MUST        | Version of the archive-time header layout.                                                              |
| `header_type`                | `uint8_enum` | 1               | MUST        | Must identify Frame Header.                                                                             |
| `volume_block_size`          | `uint32`     | 4               | MUST        | Fixed NeoTape record size for this archive volume. See §Block Size Constraints in 00-format-common.md. |
| `archive_uuid`               | `nt_uuid`    | 37              | MUST        | Stable UUID for this archive instance.                                                                  |
| `archive_name`               | `nt_name`    | 256             | SHOULD      | Human-readable archive name, in UTF-8.                                                                  |
| `volume_seq_num`             | `uint64`     | 8               | MUST        | Current archive volume sequence number.                                                                 |
| `payload_profile`            | `uint8_enum` | 1               | MUST        | Payload profile used by this archive instance.                                                          |
| `logical_slice_seq_num`      | `uint64`     | 8               | MUST        | Logical slice sequence number.                                                                          |
| `global_frame_seq_num`       | `uint64`     | 8               | MUST        | Frame sequence number scoped to the archive instance.                                                   |
| `frame_seq_num_within_slice` | `uint64`     | 8               | MUST        | Frame sequence number scoped to the logical slice.                                                      |
| `frame_payload_size`         | `uint64`     | 8               | MUST        | Meaningful bytes in this NeoTape record after the 1024-byte header.                                     |
| `frame_content_type`         | `uint8_enum` | 1               | MUST        | `SLICE_CONTENT` or `SLICE_METADATA`.                                                                |
| `frame_payload_blake3`       | `nt_hash`    | 32              | MUST        | BLAKE3 over exactly `frame_payload_size` bytes.                                                       |
| `flags`                      | `uint16`     | 2               | MUST        | Frame flags:`START`, `END`.                                                                         |
| `slice_content_size`         | `uint64`     | 8               | MUST        | Slice-level payload or metadata size; valid when `END` flag is set for `SLICE_CONTENT` or `SLICE_METADATA`, otherwise zero. |
| `slice_content_blake3`       | `nt_hash`    | 32              | MUST        | BLAKE3 over slice payload or metadata bytes; zero when `END` flag is not set for the current `frame_content_type`.        |
| `reserved`                   | `byte[*]`    | *               | MUST        | Zero bytes reserved for future fixed fields.                                                            |
| `header_crc32c`              | `nt_crc32c`  | 4               | MUST        | CRC32C for fixed header fields, excluding this field.                                                   |

## Flags

| Bit   | Name         | Meaning                                                |
| ----- | ------------ | ------------------------------------------------------ |
| 0     | `START`    | First frame of a content-type group within this slice. |
| 1     | `END`      | Last frame of a content-type group within this slice.  |
| 2–15 | _reserved_ | MUST be zero. Reserved for future use.                 |

`START` and `END` apply to the current `frame_content_type` group:

| `START` | `END` | Meaning                    |
| --------- | ------- | -------------------------- |
| 0         | 0       | Continuation frame.        |
| 0         | 1       | Last frame of this group.  |
| 1         | 0       | First frame of this group. |
| 1         | 1       | Single-frame group.        |

## Frame Payload Length Rule

`frame_payload_size` MUST be authoritative. The reader uses this length to
separate meaningful payload bytes from trailing padding inside the same NeoTape
record.

The reader MUST require:

```text
frame_payload_size <= volume_block_size - 1024
```

Bytes after `frame_payload_size` and before the end of the NeoTape record are
padding. Writers MUST write this padding as zero. Readers SHOULD validate zero
padding in strict mode and MAY ignore padding contents in salvage mode.

The writer SHOULD choose `volume_block_size` so that
`volume_block_size - 1024` is a useful bounded streaming payload chunk size.

## Content Types

Frame content type is explicit:

| Content type       | Meaning                                                          |
| ------------------ | ---------------------------------------------------------------- |
| `SLICE_CONTENT`  | Opaque bytes belonging to the logical slice content byte stream. |
| `SLICE_METADATA` | Advisory metadata bytes associated with a logical slice.         |

A normal payload reader, such as `neotape-cat-volumes`, MUST emit only
`SLICE_CONTENT` Frame payload bytes. It MUST NOT emit `SLICE_METADATA` bytes to
stdout.

A metadata-aware reader MAY parse `SLICE_METADATA` Frames for listing, partial
restore, diagnostics, or acceleration.

## Slice-Level Integrity

The `slice_content_size` and `slice_content_blake3` fields carry integrity
metadata for the current `frame_content_type` group. They are valid only on
END Frames; non-END Frames MUST set both to zero.

### SLICE_CONTENT Integrity

When `frame_content_type = SLICE_CONTENT` and the `END` flag is set:
- `slice_content_size` is the concatenated byte count of all `SLICE_CONTENT`
  payloads in the logical slice.
- `slice_content_blake3` is computed over exactly `slice_content_size` bytes
  of concatenated payload from all `SLICE_CONTENT` Frames, in Frame sequence
  order. `SLICE_METADATA` Frame bytes are NOT included.

### SLICE_METADATA Integrity

When `frame_content_type = SLICE_METADATA` and the `END` flag is set:
- `slice_content_size` is the concatenated byte count of all `SLICE_METADATA`
  payloads in the metadata group.
- `slice_content_blake3` is computed over exactly `slice_content_size` bytes
  of concatenated metadata from all `SLICE_METADATA` Frames, in Frame sequence
  order. `SLICE_CONTENT` Frame bytes are NOT included.

Both hashes are independent. A reader MUST compute each slice-level BLAKE3
directly from the relevant concatenated Frame payload bytes. Frame-level hashes
(`frame_payload_blake3`) are also independent and are not combined to form
slice-level digests.

## Optional Slice Metadata Frames

The writer MAY follow the last `SLICE_CONTENT` Frame of a logical slice with
zero or more `SLICE_METADATA` Frames. Each such Frame carries bytes from a
restricted ar archive conforming to the ar subset format defined in
[docs/spec/00-format-common.md](00-format-common.md#ar-subset-format).

`SLICE_METADATA` Frames are NeoTape transport metadata associated with a logical
slice. They are not part of the payload stream and MUST NOT be emitted to stdout
by `neotape-cat-volumes`.

`SLICE_METADATA` Frames are advisory. A reader MUST NOT reject a logical slice or
archive solely because of missing, truncated, or corrupt `SLICE_METADATA` Frames,
unless the selected operation explicitly requires slice metadata. If
`frame_payload_blake3` verification fails for a `SLICE_METADATA` Frame, the
reader SHOULD log a warning and continue in normal payload extraction mode.

If EOT occurs before all `SLICE_METADATA` Frames can be committed, the next
volume SHOULD resume with a Volume Header followed by the next complete
`SLICE_METADATA` Frame for the same `logical_slice_seq_num`. No partial Frame is
continued across the volume boundary.

The specific ar archive member names and their semantics are intentionally left
to a future specification.

## Volume Boundary Rule

NeoTape uses complete Frames as the smallest committed content/metadata transport
unit.

If EOT or a write error occurs while writing a Frame, that Frame MUST be treated
as uncommitted unless the backend can prove that the entire `volume_block_size`
record was written successfully. The writer resumes on the next volume by
writing a Volume Header (will first initialize Medium Header if not exist) followed by the next complete Frame that has not been committed.

There is no offset field and no partial Frame continuation mechanism in this
design.
