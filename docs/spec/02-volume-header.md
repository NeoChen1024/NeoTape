# Volume Header

Status: draft / field inventory.

The Volume Header is the first archive-time record for one archive volume. It
binds an archive instance to a physical medium position and declares the fixed
NeoTape record size used by that archive volume.

The field inventory below defines the current proposed datatypes and encoded
field sizes. Exact byte offsets, enum numeric assignments, and future reserved
field allocation remain open until the byte layout is frozen.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-format-common.md](00-format-common.md).

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

| Field                 | datatype      | size (in bytes) | Requirement | Notes                                                                       |
| --------------------- | ------------- | --------------- | ----------- | --------------------------------------------------------------------------- |
| `magic`               | `char[8]`     | 8               | MUST        | Fixed NeoTape identifier: `NeoTape\0`.                                      |
| `header_version`      | `uint8`       | 1               | MUST        | Version of the archive-time header layout.                                  |
| `header_type`         | `uint8_enum`  | 1               | MUST        | Must identify Volume Header.                                                |
| `volume_block_size`   | `uint32`      | 4               | MUST        | Fixed NeoTape record size for this archive volume. See §Block Size Constraints in 00-format-common.md. |
| `archive_uuid`        | `nt_uuid`     | 37              | MUST        | Stable UUID for this archive instance.                                      |
| `archive_name`        | `nt_name`     | 256             | SHOULD      | Human-readable archive name, in UTF-8.                                      |
| `volume_seq_num`      | `uint64`      | 8               | MUST        | Volume sequence number within `archive_uuid`, starting at 1.                |
| `payload_profile`     | `uint8_enum`  | 1               | MUST        | Payload profile used by this archive instance.                              |
| `volume_write_at_utc` | `nt_time`     | 20              | MUST        | Volume write timestamp using the fixed NeoTape timestamp format.            |
| `flags`               | `uint16`      | 2               | SHOULD      | Reserved feature or compatibility flags.                                    |
| `reserved`            | `byte[*]`     | *               | MUST        | Zero bytes reserved for future fixed fields.                                |
| `header_crc32c`       | `nt_crc32c`   | 4               | MUST        | CRC32C for fixed header fields, excluding this field.                       |

For a total of 1024 bytes.

## Block Size Rule

`volume_block_size` is the fixed NeoTape record size for this archive volume.
It is not a recommendation. After the Volume Header is committed, the writer
MUST use this `volume_block_size` for all NeoTape records in the same archive
volume.

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
