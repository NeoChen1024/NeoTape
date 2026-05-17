# Archive End Header

Status: field sizes and datatypes are concrete.

The Archive End Header is the final cleanly completed archive record. It
declares that an archive instance is complete. It fits in a single 1024-byte
fixed header with a trailing CRC32C.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-format-common.md](00-format-common.md).

## Placement

The Archive End Header resides in its own archive-end tape file, delimited by
filemarks like Volume Headers and completed logical slices.

The Archive End Header is an archive-time commit record and MUST fit within one
NeoTape record.

## Fixed Fields

The fixed fields should be enough to identify the archive instance, summarize
the final sequence position, name the archive for human inspection, and mark
the archive as cleanly complete.

| Field                         | datatype      | size (in bytes) | Requirement | Notes                                                                       |
| ----------------------------- | ------------- | --------------- | ----------- | --------------------------------------------------------------------------- |
| `magic`                       | `char[8]`     | 8               | MUST        | Fixed NeoTape identifier: `NeoTape\0`.                                      |
| `header_version`              | `uint8`       | 1               | MUST        | Version of the archive-time header layout.                                  |
| `header_type`                 | `uint8_enum`  | 1               | MUST        | Must identify Archive End Header.                                           |
| `volume_block_size`           | `uint32`      | 4               | MUST        | Fixed NeoTape record size for this archive volume. See §Block Size Constraints in 00-format-common.md. |
| `archive_uuid`                | `nt_uuid`     | 37              | MUST        | Stable UUID for this archive instance.                                      |
| `archive_name`                | `nt_name`     | 256             | SHOULD      | Human-readable archive name, in UTF-8.                                      |
| `volume_seq_num`              | `uint64`      | 8               | MUST        | Final archive volume sequence number.                                       |
| `payload_profile`             | `uint8_enum`  | 1               | MUST        | Payload profile used by this archive instance.                              |
| `last_logical_slice_seq_num`  | `uint64`      | 8               | MUST        | Last completed logical slice sequence number.                               |
| `last_global_frame_seq_num`   | `uint64`      | 8               | MUST        | Last Frame sequence number scoped to the archive instance.                  |
| `created_by_implementation`   | `char[64]`    | 64              | SHOULD      | Writer implementation name and version.                                     |
| `created_by_build_id`         | `char[64]`    | 64              | MAY         | Source revision, build ID, or other diagnostic identifier. May be empty.    |
| `archive_end_at_utc`          | `nt_time`     | 20              | MUST        | Archive end timestamp using the fixed NeoTape timestamp format.             |
| `flags`                       | `uint16`      | 2               | MUST        | Archive end flags: `CLEAN_END`, `CATALOG_PRESENT`.                         |
| `reserved`                    | `byte[*]`     | *               | MUST        | Zero bytes reserved for future fixed fields.                                |
| `header_crc32c`               | `nt_crc32c`   | 4               | MUST        | CRC32C for fixed header fields, excluding this field.                       |

For a total of 1024 bytes.

## Flags

| Bit  | Name                | Meaning                                                                                                  |
| ---- | ------------------- | -------------------------------------------------------------------------------------------------------- |
| 0    | `CLEAN_END`       | Archive completed cleanly. MUST be 1. A value of 0 means the writer did not produce a clean archive end. |
| 1    | `CATALOG_PRESENT` | Archive-level catalog metadata is present in the archive before this Archive End Header.                  |
| 2-15 | _reserved_        | MUST be zero. Reserved for future use.                                                                   |

`CLEAN_END` is the authoritative indicator of a cleanly completed archive. If a
reader finds an Archive End Header with `CLEAN_END` clear (bit 0 = 0), the
archive MUST NOT be treated as cleanly complete.

`CATALOG_PRESENT` indicates that the writer included archive-level catalog
metadata before the Archive End Header. The reader SHOULD attempt to locate and
read it. If `CATALOG_PRESENT` is set but the catalog metadata is missing or
corrupt, the reader MUST NOT reject the archive (catalog metadata is advisory).

Archive-level catalog metadata is not part of the Archive End Header and is not
protected by the Archive End Header CRC32C.

## Completion Rule

If a reader does not find a valid Archive End Header, the archive MUST NOT be
treated as cleanly complete, even if all expected logical slices have been read
and their Frame Headers validated.

Archive-level completion is declared only by a valid Archive End Header with the
`CLEAN_END` flag set.

Because NeoTape core framing is length-based, the Archive End Header does not
depend on pax/tar EOA detection. Payload-profile-specific end markers,
including pax EOA, remain inside payload bytes and are interpreted only by the
relevant payload profile or downstream extractor.
