# Slice Trailer

Status: draft / field inventory.

The Slice Trailer finalizes one logical slice. It records the actual logical
slice payload size and BLAKE3 digest after the writer has committed the final
segment of that slice.

The exact binary layout, datatype mapping, and fixed field sizes are
intentionally left open in this draft. Field tables include empty `datatype` and
`size (in bytes)` columns so those decisions can be made explicitly later.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-header-common.md](00-header-common.md).

## Placement

Every logical slice MUST be followed by a NeoTape Slice Trailer after the writer
has committed the final segment for that logical slice.

The Slice Trailer is the next committed NeoTape record after that logical slice
payload completes. It may physically reside on a later medium if EOT occurs
before the trailer can be committed.

The Slice Trailer is NeoTape transport metadata and is not part of the payload
stream. It MUST NOT be emitted to stdout by `neotape-cat-volumes`.

The Slice Trailer fixed header MUST fit within one tape record. Metadata after
the fixed header may span additional records or continuation segments.

## Fixed Fields

The fixed fields should be enough to identify the archive instance, identify the
logical slice, verify the exact payload byte range, and locate optional trailer
metadata.

| Field | datatype | size (in bytes) | Requirement | Notes |
| --- | --- | --- | --- | --- |
| magic | char[8] | 8 | MUST | Fixed NeoTape identifier: `NeoTape\0`. |
| header_version |  |  | MUST | Version of the archive-time header layout. |
| header_type |  |  | MUST | Must identify Slice Trailer. |
| header_size |  |  | MUST | Encoded fixed header size. |
| header_crc32c |  |  | MUST | CRC32C for fixed header fields, excluding this field. |
| archive_uuid |  |  | MUST | Stable UUID for this archive instance. |
| tape_seq_num |  |  | MUST | Current archive volume sequence number. |
| logical_slice_seq_num |  |  | MUST | Logical slice sequence number. |
| last_segment_seq_num_within_slice |  |  | MUST | Last segment sequence number scoped to this logical slice. |
| last_global_segment_seq_num |  |  | SHOULD | Last segment sequence number scoped to the archive instance. |
| slice_payload_size |  |  | MUST | Exact number of logical slice payload bytes. |
| slice_payload_blake3 |  |  | MUST | BLAKE3 over exactly `slice_payload_size` logical slice payload bytes. |
| slice_catalog_present |  |  | SHOULD | Indicates whether slice-local catalog metadata is present. |
| slice_catalog_size |  |  | MAY | Size of slice-local catalog metadata when present. |
| slice_catalog_blake3 |  |  | MAY | BLAKE3 for slice-local catalog metadata when present. |
| metadata_total_size |  |  | SHOULD | Exact metadata byte count after the fixed header, excluding filemarks and padding. |
| metadata_block_size |  |  | SHOULD | Metadata block payload size when metadata blocks are present. |
| metadata_record_count |  |  | SHOULD | Number of metadata records or blocks when metadata is present. |
| metadata_blake3 |  |  | SHOULD | BLAKE3 over the reconstructed metadata byte string. |
| metadata_format_version |  |  | SHOULD | Version of the trailer metadata framing. |
| flags |  |  | SHOULD | Reserved feature or compatibility flags. |
| reserved |  |  | MUST | Zero bytes reserved for future fixed fields. |

## Payload Verification

`slice_payload_blake3` is computed over exactly `slice_payload_size` bytes of
concatenated logical slice payload, excluding the Slice Trailer itself.

NeoTape core does not inspect payload bytes to find an end marker. For the
NeoTape/PAX payload profile, the payload may be an arbitrary range of a larger
pax stream and does not need to end with pax EOA.

## Metadata Area

Slice Trailer metadata is stored in a length-framed metadata area following the
fixed header. The metadata area MAY contain slice-local catalog data, warnings,
source-read diagnostics, payload-profile information, and other advisory
metadata.

Metadata Blocks MAY span multiple tape records and MAY continue across physical
segments or volumes using `TRAILER_METADATA` continuation segments. A
continuation segment with `segment_content_type = TRAILER_METADATA` is not
payload and MUST NOT be emitted to stdout.

If EOT occurs in the middle of a metadata area, the next volume MUST resume with
a Volume Header followed by the next `TRAILER_METADATA` segment or fail the
archive as not cleanly complete.

Slice-local catalog data is an index or hint and MUST NOT replace the
authoritative payload metadata, such as pax entries in the NeoTape/PAX profile.
