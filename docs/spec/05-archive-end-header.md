# Archive End Header

Status: draft / field inventory.

The Archive End Header is the final cleanly completed archive record. It
declares that an archive instance is complete.

The exact binary layout, datatype mapping, and fixed field sizes are
intentionally left open in this draft. Field tables include empty `datatype` and
`size (in bytes)` columns so those decisions can be made explicitly later.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-header-common.md](00-header-common.md).

## Placement

The Archive End Header resides in its own archive-end tape file, delimited by
filemarks like Volume Headers and completed logical slices.

The Archive End Header is an archive-time commit record and MUST fit within one
tape record.

## Fixed Fields

The fixed fields should be enough to identify the archive instance, summarize
the final sequence position, validate optional final catalog metadata, and mark
the archive as cleanly complete.

| Field | datatype | size (in bytes) | Requirement | Notes |
| --- | --- | --- | --- | --- |
| magic | char[8] | 8 | MUST | Fixed NeoTape identifier: `NeoTape\0`. |
| header_version |  |  | MUST | Version of the archive-time header layout. |
| header_type |  |  | MUST | Must identify Archive End Header. |
| header_size |  |  | MUST | Encoded fixed header size. |
| header_crc32c |  |  | MUST | CRC32C for fixed header fields, excluding this field. |
| archive_uuid |  |  | MUST | Stable UUID for this archive instance. |
| tape_seq_num |  |  | MUST | Final archive volume sequence number. |
| last_logical_slice_seq_num |  |  | MUST | Last completed logical slice sequence number. |
| last_global_segment_seq_num |  |  | SHOULD | Last segment sequence number scoped to the archive instance. |
| clean_end |  |  | MUST | Must indicate a clean archive completion. |
| catalog_present |  |  | SHOULD | Indicates whether archive-level catalog metadata is present. |
| catalog_blake3 |  |  | MAY | BLAKE3 for archive-level catalog metadata when present. |
| writer_version |  |  | SHOULD | Writer implementation name and version. |
| archive_end_timestamp_utc |  |  | MUST | Archive end timestamp using the fixed NeoTape timestamp format. |
| flags |  |  | SHOULD | Reserved feature or compatibility flags. |
| reserved |  |  | MUST | Zero bytes reserved for future fixed fields. |

## Completion Rule

If a reader does not find a valid Archive End Header, the archive MUST NOT be
treated as cleanly complete, even if all expected logical slices and Slice
Trailers have been read.

Archive-level completion is declared only by a valid Archive End Header.

Because NeoTape core framing is length-based, the Archive End Header does not
depend on pax/tar EOA detection. Payload-profile-specific end markers, including
pax EOA, remain inside payload bytes and are interpreted only by the relevant
payload profile or downstream extractor.
