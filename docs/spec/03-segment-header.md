# Segment Header

Status: draft / field inventory.

The Segment Header begins each length-framed physical segment inside a logical
slice tape file. It tells a reader how many payload bytes follow and how those
bytes contribute to the current logical slice.

The exact binary layout, datatype mapping, and fixed field sizes are
intentionally left open in this draft. Field tables include empty `datatype` and
`size (in bytes)` columns so those decisions can be made explicitly later.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-header-common.md](00-header-common.md).

## Placement

A Segment Header is written at the beginning of each physical segment inside a
slice tape file. The first Segment Header usually occupies the first NeoTape
record in that slice tape file.

Subsequent Segment Headers are located by advancing past the current
Segment Header's 1024-byte fixed area plus the payload bytes that follow. For a
non-continuation segment (`segment_offset = 0`) this is `segment_payload_size`.
For a continuation segment (`segment_offset > 0`) this is `segment_payload_size
- segment_offset`. Readers MUST use explicit length fields, not payload contents
or filemark position, to locate the next Segment Header.

The Segment Header is an archive-time commit record and MUST fit within one tape
record.

## Fixed Fields

The fixed fields should be enough to identify the archive instance, identify the
logical slice, sequence the segment, describe the following byte range, and
validate the header.

| Field                        | datatype   | size (in bytes) | Requirement | Notes                                                                             |
| ---------------------------- | ---------- | --------------- | ----------- | --------------------------------------------------------------------------------- |
| magic                        | char[8]    | 8               | MUST        | Fixed NeoTape identifier:`NeoTape\0`.                                           |
| header_version               | uint8      | 1               | MUST        | Version of the archive-time header layout.                                        |
| header_type                  | uint8      | 1               | MUST        | Must identify Segment Header.                                                     |
| archive_uuid                 | nt_uuid    | 37              | MUST        | Stable UUID for this archive instance.                                            |
| volume_seq_num               | uint32     | 4               | MUST        | Current archive volume sequence number.                                           |
| logical_slice_seq_num        | uint32     | 4               | MUST        | Logical slice sequence number.                                                    |
| segment_seq_num_within_slice | uint32     | 4               | MUST        | Segment sequence number scoped to the logical slice.                              |
| global_segment_seq_num       | uint32     | 4               | MUST        | Segment sequence number scoped to the archive instance.                           |
| segment_payload_size         | uint64     | 8               | MUST        | Exact number of payload bytes following this Segment Header.                      |
| segment_offset               | uint64     | 8               | MUST        | Cumulative payload bytes of this segment already committed on prior volumes. MUST be 0 unless the segment continues across a volume boundary. |
| segment_content_type         | uint8_enum | 1               | MUST        | `PAYLOAD` or `TRAILER_METADATA`.                                              |
| payload_profile              | uint8      | 1               | SHOULD      | Payload profile identifier such as pax, raw, or a future profile.                 |
| segment_payload_blake3       | nt_hash    | 32              | MUST        | BLAKE3 over this segment payload byte range when recorded.                        |
| flags                        | uint16     | 2               | MUST        | Segment flags such as `SLICE_START`, `SLICE_CONTINUATION`, and `SLICE_END`. |
| slice_payload_size           | uint64     | 8               | MUST        | Slice-level payload size; valid only when SLICE_END flag is set, otherwise zero.  |
| slice_payload_blake3         | nt_hash    | 32              | MUST        | BLAKE3 over slice_payload_size; zero when SLICE_END flag is not set.              |
| reserved                     | byte[*]    | *               | MUST        | Zero bytes reserved for future fixed fields.                                      |
| header_crc32c                | nt_crc32c  | 4               | MUST        | CRC32C for fixed header fields, excluding this field.                             |

## Payload Length Rule

`segment_payload_size` MUST be authoritative. The reader uses this length to
determine the end of the segment payload and the location of the next Segment
Header inside the same slice tape file.

The `segment_payload_size` SHOULD normally match the writer's bounded streaming
buffer size, except for the final segment of a logical slice.

**Recommendation:** To avoid wasting trailing record space, writers SHOULD set
`segment_payload_size` so that the segment consumes a whole number of tape
records. Given the 1024-byte fixed header, a convenient formula is
`segment_payload_size` = `segment_size` − 1024, where `segment_size` is a
multiple of `volume_block_size`.

## Segment Continuation

If EOT occurs partway through a segment's payload, the writer MAY continue
writing the remaining payload on the next volume. The continuation uses a new
Segment Header in the next volume's tape file, with the following rules:

- `segment_seq_num_within_slice`, `global_segment_seq_num`, and
  `segment_payload_size` MUST match the original interrupted segment header.
- `segment_offset` MUST be set to the number of payload bytes of this segment
  that were already committed on prior volumes.
- The current record carries exactly `segment_payload_size - segment_offset`
  payload bytes. The reader advances past this many bytes to locate the next
  Segment Header.
- `segment_payload_blake3` MUST be the BLAKE3 of the segment's full payload
  (bytes from all volumes concatenated), not just the continuation portion.
- `segment_offset` MUST be 0 for segments that are not a volume-boundary
  continuation. A reader SHOULD treat a non-zero `segment_offset` on the first
  volume of an archive as a format error.

## Content Types

Segment payload content type SHOULD be explicit:

| Content type         | Meaning                                                  |
| -------------------- | -------------------------------------------------------- |
| `PAYLOAD`          | Opaque bytes belonging to the current logical slice.     |
| `TRAILER_METADATA` | Advisory metadata not part of the logical slice payload. |

## Slice-Level Integrity

The segment header with SLICE_END flag carries the authoritative
`slice_payload_size` and `slice_payload_blake3` for the logical slice.

`slice_payload_blake3` is computed over exactly `slice_payload_size` bytes of
concatenated payload from all PAYLOAD segments in the logical slice, in segment
sequence order. TRAILER_METADATA segment bytes are NOT included in the
slice-level BLAKE3.

Segments without SLICE_END MUST set both `slice_payload_size` and
`slice_payload_blake3` to zero.

Segment-level hashes (`segment_payload_blake3`) are independent and are not
combined to form the slice-level digest. A reader MUST compute the slice-level
BLAKE3 directly from the concatenated PAYLOAD segment payloads.

## Optional Trailer Metadata

The writer MAY follow the last PAYLOAD segment of a logical slice with zero or
more `TRAILER_METADATA` segments (see
[docs/spec/03-segment-header.md](03-segment-header.md#content-types)). Each such
segment carries a restricted ar archive conforming to the ar subset format
defined in [docs/spec/00-header-common.md](00-header-common.md#ar-subset-format).

A `TRAILER_METADATA` segment is NeoTape transport metadata and is not part of
the payload stream. It MUST NOT be emitted to stdout by
`neotape-cat-volumes`.

`TRAILER_METADATA` segments are advisory. A reader MUST NOT reject a logical
slice or archive solely because of a missing, truncated, or corrupt
`TRAILER_METADATA` segment. If `segment_payload_blake3` verification fails for a
`TRAILER_METADATA` segment, the reader SHOULD log a warning and continue.

If EOT occurs before all `TRAILER_METADATA` segments can be committed, the next
volume SHOULD resume with a Volume Header followed by a continuation
`TRAILER_METADATA` segment. The standard segment continuation mechanism
(`segment_seq_num_within_slice`, `segment_content_type`) provides the framing
for this case.

The specific ar archive member names and their semantics are intentionally left
to a future specification.
