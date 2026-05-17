# Frame Header

Status: draft / field inventory.

A NeoTape Frame is the self-describing transport record used to carry payload
bytes or slice catalog bytes inside an archive volume. Each Frame occupies
exactly one NeoTape record of `volume_block_size` bytes. The first 1024 bytes of
that record are the Frame Header; the remaining bytes contain frame payload
followed by zero padding.

The Frame Header tells a reader how many bytes in the current record are
meaningful, how those bytes contribute to the current logical slice, and whether
the bytes belong to the payload stream or to advisory catalog data.

The exact binary layout, datatype mapping, and fixed field sizes are
intentionally left open in this draft. Field tables include empty `datatype` and
`size (in bytes)` columns so those decisions can be made explicitly later.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-header-common.md](00-header-common.md).

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

| Field                      | datatype   | size (in bytes) | Requirement | Notes                                                                            |
| -------------------------- | ---------- | --------------- | ----------- | -------------------------------------------------------------------------------- |
| magic                      | char[8]    | 8               | MUST        | Fixed NeoTape identifier: `NeoTape\0`.                                          |
| header_version             | uint8      | 1               | MUST        | Version of the archive-time header layout.                                       |
| header_type                | uint8      | 1               | MUST        | Must identify Frame Header.                                                      |
| archive_uuid               | nt_uuid    | 37              | MUST        | Stable UUID for this archive instance.                                           |
| volume_seq_num             | uint32     | 4               | MUST        | Current archive volume sequence number.                                          |
| logical_slice_seq_num      | uint32     | 4               | MUST        | Logical slice sequence number.                                                   |
| frame_seq_num_within_slice | uint32     | 4               | MUST        | Frame sequence number scoped to the logical slice.                               |
| global_frame_seq_num       | uint32     | 4               | MUST        | Frame sequence number scoped to the archive instance.                            |
| frame_payload_size         | uint64     | 8               | MUST        | Meaningful payload bytes in this NeoTape record after the 1024-byte header.      |
| frame_content_type         | uint8_enum | 1               | MUST        | `PAYLOAD` or `SLICE_CATALOG`.                                                    |
| payload_profile            | uint8      | 1               | SHOULD      | Payload profile identifier such as pax, raw, or a future profile.                |
| frame_payload_blake3       | nt_hash    | 32              | MUST        | BLAKE3 over exactly `frame_payload_size` bytes.                                  |
| flags                      | uint16     | 2               | MUST        | Frame flags such as `SLICE_START`, `SLICE_END`, `CATALOG_START`, `CATALOG_END`.  |
| slice_payload_size         | uint64     | 8               | MUST        | Slice-level payload size; valid only when `SLICE_END` flag is set, otherwise zero. |
| slice_payload_blake3       | nt_hash    | 32              | MUST        | BLAKE3 over slice payload bytes; zero when `SLICE_END` flag is not set.          |
| reserved                   | byte[*]    | *               | MUST        | Zero bytes reserved for future fixed fields.                                     |
| header_crc32c              | nt_crc32c  | 4               | MUST        | CRC32C for fixed header fields, excluding this field.                            |

## Payload Length Rule

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

Frame payload content type is explicit:

| Content type      | Meaning                                                              |
| ----------------- | -------------------------------------------------------------------- |
| `PAYLOAD`         | Opaque bytes belonging to the logical slice payload byte stream.     |
| `SLICE_CATALOG`   | Advisory catalog bytes associated with a logical slice.              |

A normal payload reader, such as `neotape-cat-volumes`, MUST emit only
`PAYLOAD` Frame payload bytes. It MUST NOT emit `SLICE_CATALOG` bytes to
stdout.

A catalog-aware reader MAY parse `SLICE_CATALOG` Frames for listing, partial
restore, diagnostics, or acceleration.

## Slice-Level Integrity

The Frame Header with `SLICE_END` flag carries the authoritative
`slice_payload_size` and `slice_payload_blake3` for the logical slice.

`slice_payload_blake3` is computed over exactly `slice_payload_size` bytes of
concatenated payload from all `PAYLOAD` Frames in the logical slice, in Frame
sequence order. `SLICE_CATALOG` Frame bytes are NOT included in the slice-level
BLAKE3.

Frames without `SLICE_END` MUST set both `slice_payload_size` and
`slice_payload_blake3` to zero.

Frame-level hashes (`frame_payload_blake3`) are independent and are not combined
to form the slice-level digest. A reader MUST compute the slice-level BLAKE3
directly from the concatenated `PAYLOAD` Frame payload bytes.

## Optional Slice Catalog Frames

The writer MAY follow the last `PAYLOAD` Frame of a logical slice with zero or
more `SLICE_CATALOG` Frames. Each such Frame carries bytes from a restricted ar
archive conforming to the ar subset format defined in
[docs/spec/00-header-common.md](00-header-common.md#ar-subset-format).

`SLICE_CATALOG` Frames are NeoTape transport metadata associated with a logical
slice. They are not part of the payload stream and MUST NOT be emitted to stdout
by `neotape-cat-volumes`.

`SLICE_CATALOG` Frames are advisory. A reader MUST NOT reject a logical slice or
archive solely because of missing, truncated, or corrupt `SLICE_CATALOG` Frames,
unless the selected operation explicitly requires catalog data. If
`frame_payload_blake3` verification fails for a `SLICE_CATALOG` Frame, the
reader SHOULD log a warning and continue in normal payload extraction mode.

If EOT occurs before all `SLICE_CATALOG` Frames can be committed, the next
volume SHOULD resume with a Volume Header followed by the next complete
`SLICE_CATALOG` Frame for the same `logical_slice_seq_num`. No partial Frame is
continued across the volume boundary.

The specific ar archive member names and their semantics are intentionally left
to a future specification.

## Volume Boundary Rule

NeoTape uses complete Frames as the smallest committed payload/catalog transport
unit.

If EOT or a write error occurs while writing a Frame, that Frame MUST be treated
as uncommitted unless the backend can prove that the entire `volume_block_size`
record was written successfully. The writer resumes on the next volume by
writing a Volume Header followed by the next complete Frame that has not been
committed.

This rule replaces partial-segment continuation. There is no `segment_offset`
field and no partial Frame continuation mechanism in this design.
