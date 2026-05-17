# Volume Header

Status: draft / field inventory.

The Volume Header is the first archive-time record for one archive volume. It
binds an archive instance to a physical medium position and declares the fixed
NeoTape record size used by that archive volume.

The exact binary layout, datatype mapping, and fixed field sizes are
intentionally left open in this draft. Field tables include empty `datatype` and
`size (in bytes)` columns so those decisions can be made explicitly later.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-header-common.md](00-header-common.md).

## Placement

A Volume Header is written at the beginning of each archive volume.

For an initialized physical medium, the first archive instance usually begins
in the tape file immediately after the Medium Header. If multiple archive
instances are appended to the same physical medium, later Volume Headers begin
at the tape file immediately after the previous archive's clean Archive End
Header.

The Volume Header is an archive-time commit record and MUST fit within one tape
record.

## Fixed Fields

The fixed fields should be readable with minimal parser state. They should be
enough to identify the archive instance, validate the header, sequence the
volume, and configure record framing for the rest of the archive volume.

| Field | datatype | size (in bytes) | Requirement | Notes |
| --- | --- | --- | --- | --- |
| magic | char[8] | 8 | MUST | Fixed NeoTape identifier: `NeoTape\0`. |
| header_version |  |  | MUST | Version of the archive-time header layout. |
| header_type |  |  | MUST | Must identify Volume Header. |
| header_size |  |  | MUST | Encoded fixed header size. |
| header_crc32c |  |  | MUST | CRC32C for fixed header fields, excluding this field. |
| archive_uuid |  |  | MUST | Stable UUID for this archive instance. |
| tape_seq_num |  |  | MUST | Volume sequence number within `archive_uuid`, starting at 1. |
| volume_label |  |  | SHOULD | Human-facing label for this archive volume. |
| format_version |  |  | MUST | Archive/container format version used by this archive instance. |
| block_size |  |  | MUST | Fixed NeoTape record size for this archive volume. |
| archive_write_timestamp_utc |  |  | MUST | Archive write timestamp using the fixed NeoTape timestamp format. |
| archive_sequence_on_media |  |  | MAY | Sequence of this archive instance on the physical medium, if known. |
| flags |  |  | SHOULD | Reserved feature or compatibility flags. |
| reserved |  |  | MUST | Zero bytes reserved for future fixed fields. |

## Block Size Rule

`block_size` is the fixed NeoTape record size for this archive volume. It is not
a recommendation. After the Volume Header is committed, the writer MUST use this
`block_size` for all NeoTape records in the same archive volume.

A reader SHOULD treat a record-size change inside the volume as a format error
unless an explicit future extension allows it.

## Excluded Mutable State

The Volume Header SHOULD NOT record fields that are only known at the end of an
archive or that would require backfilling, including:

- `is_last_volume`
- `total_volumes`
- `archive_total_size`
- `payload_total_size`

Archive completion is declared by the Archive End Header.
