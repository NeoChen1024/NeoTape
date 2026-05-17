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

Subsequent Segment Headers are located by adding the previous
`segment_payload_size` to the end of the previous Segment Header. Readers MUST
use explicit length fields, not payload contents or filemark position, to locate
the next Segment Header or the following Slice Trailer.

The Segment Header is an archive-time commit record and MUST fit within one tape
record.

## Fixed Fields

The fixed fields should be enough to identify the archive instance, identify the
logical slice, sequence the segment, describe the following byte range, and
validate the header.

| Field | datatype | size (in bytes) | Requirement | Notes |
| --- | --- | --- | --- | --- |
| magic | char[8] | 8 | MUST | Fixed NeoTape identifier: `NeoTape\0`. |
| header_version |  |  | MUST | Version of the archive-time header layout. |
| header_type |  |  | MUST | Must identify Segment Header. |
| header_size |  |  | MUST | Encoded fixed header size. |
| header_crc32c |  |  | MUST | CRC32C for fixed header fields, excluding this field. |
| archive_uuid |  |  | MUST | Stable UUID for this archive instance. |
| tape_seq_num |  |  | MUST | Current archive volume sequence number. |
| logical_slice_seq_num |  |  | MUST | Logical slice sequence number. |
| segment_seq_num_within_slice |  |  | MUST | Segment sequence number scoped to the logical slice. |
| global_segment_seq_num |  |  | SHOULD | Segment sequence number scoped to the archive instance. |
| segment_payload_size |  |  | MUST | Exact number of payload bytes following this Segment Header. |
| segment_payload_offset_within_slice |  |  | MUST | Logical offset of this segment payload within the current slice. |
| segment_content_type |  |  | MUST | `PAYLOAD` or `TRAILER_METADATA`. |
| payload_profile |  |  | SHOULD | Payload profile identifier such as pax, raw, or a future profile. |
| segment_payload_blake3 |  |  | MAY | BLAKE3 over this segment payload byte range when recorded. |
| flags |  |  | MUST | Segment flags such as `SLICE_START`, `SLICE_CONTINUATION`, and `SLICE_END_HINT`. |
| reserved |  |  | MUST | Zero bytes reserved for future fixed fields. |

## Payload Length Rule

`segment_payload_size` MUST be authoritative. The reader uses this length to
determine the end of the segment payload and the location of the next Segment
Header or Slice Trailer inside the same slice tape file.

The `segment_payload_size` SHOULD normally match the writer's bounded streaming
buffer size, except for the final segment of a logical slice.

## Content Types

Segment payload content type SHOULD be explicit:

| Content type | Meaning |
| --- | --- |
| `PAYLOAD` | Opaque bytes belonging to the current logical slice. |
| `TRAILER_METADATA` | Slice Trailer metadata continuation, not part of the logical slice payload. |

If a segment records optional payload integrity metadata, it SHOULD use BLAKE3
over the committed payload byte range. Slice-level integrity remains
authoritative at the Slice Trailer level via `slice_payload_blake3`.
