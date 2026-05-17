# Medium Header

Status: draft / field inventory.

The Medium Header is the immutable metadata area written at BOT when a physical
medium is initialized. It is not an archive table of contents and must not be
updated after later archive instances are appended.

The exact binary layout, datatype mapping, and fixed field sizes are
intentionally left open in this draft. Field tables include empty `datatype` and
`size (in bytes)` columns so those decisions can be made explicitly later.

## Common Rules

Requirement keyword handling, empty fixed-field encoding, CRC32C calculation,
and timestamp encoding are defined in
[docs/spec/00-header-common.md](00-header-common.md).

## Placement

A physical NeoTape medium MUST begin at BOT with a NeoTape Medium Header in tape
file 0, followed by a filemark.

Unlike archive-time fixed headers, the Medium Header MAY span multiple NeoTape
records. The first record MUST contain enough fixed binary/ASCII information to
identify NeoTape, identify the Medium Header format, locate the remaining
Medium Header data, and verify the remaining records.

Volume, Frame, and Archive End headers are archive-time commit records and must
fit within a single NeoTape record. The Medium Header is the only v0.1 header
type that may span multiple NeoTape records.

## Fixed Fields

The fixed fields should be readable with minimal parser state. They should be
enough to reject unrelated media, identify the Medium Header version, describe
the physical medium at initialization time, and determine the ar metadata bundle
size.

These fields are immutable descriptive metadata, not an append-time state index.

| Field                     | datatype  | size (in bytes) | Requirement | Notes                                                                                         |
| ------------------------- | --------- | --------------- | ----------- | --------------------------------------------------------------------------------------------- |
| magic                     | char[8]   | 8               | MUST        | Fixed NeoTape identifier: `NeoTape\0`.                                                       |
| header_version            | uint8     | 1               | MUST        | Version of the Medium Header layout, independent from archive format versions.                |
| header_type               | uint8     | 1               | MUST        | Must identify Medium Header.                                                                  |
| medium_header_block_size  | uint32    | 4               | MUST        | Block size of the Medium Header. Must be at least 64 KiB.                                     |
| medium_header_block_count | uint16    | 2               | MUST        | Number of blocks occupied by the Medium Header known at write time.                           |
| flags                     | uint16    | 2               | SHOULD      | Reserved feature or compatibility flags.                                                      |
| medium_uuid               | nt_uuid   | 37              | MUST        | Stable UUID for this initialized NeoTape physical medium.                                     |
| initialized_at_utc        | nt_time   | 20              | MUST        | UTC initialization timestamp. Uses the fixed NeoTape timestamp format.                        |
| medium_label              | byte[256] | 256             | SHOULD      | Human label. UTF-8. May be much longer than LTO MAM labels; 255 bytes is a reasonable target. |
| created_by_implementation | char[64]  | 64              | SHOULD      | Writer implementation name and version.                                                       |
| created_by_build_id       | char[64]  | 64              | MAY         | Source revision, build ID, or other diagnostic identifier. May be empty.                      |
| metadata_bundle_size      | uint32    | 4               | MUST        | Exact size of the ar metadata bundle.                                                         |
| metadata_bundle_blake3    | nt_hash   | 32              | SHOULD      | Integrity hash for the ar metadata bundle bytes.                                              |
| reserved                  | byte[*]   | *               | MUST        | Zero bytes. Reserved for future fixed fields.                                                 |
| medium_header_crc32c      | nt_crc32c | 4               | MUST        | CRC32C for the 1024-byte fixed Medium Header area, excluding this field.                      |

For a total of 1024 bytes. Future versions MAY allocate fields from the reserved
space, but version 1 writers MUST write it as zero bytes and version 1 readers
MUST include those zero bytes in the CRC32C calculation.

The total Medium Header byte size is not stored as a separate field. It is
always derived as:

```text
1024 + metadata_bundle_size
```

## Metadata Bundle

The Medium Header metadata bundle MUST be a restricted classic ar archive. It is
a flat collection of named byte blobs, not a filesystem archive.

The ar metadata bundle immediately follows the fixed Medium Header fields.
There is no metadata bundle offset field.

NeoTape does not impose a separate fixed upper size limit on the Medium Header
metadata bundle. It may span multiple blocks as part of the Medium Header.
Practical limits come from the encoded header length fields, the ar member
format, implementation resource limits, and available medium capacity.

The ar container itself SHOULD NOT be compressed. Individual members MAY be
compressed when that is useful, for example a minimal reader source package as a
`.tar.gz` member.

The exact member set is intentionally left to later specification work.

Potential member files:

| Member name   | Requirement | Suggested content                                                                                                  | Notes                                                                               |
| ------------- | ----------- | ------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------- |
| README        | MAY         | ASCII Text. Human-facing description or restore notes.                                                             | Plain text is preferred.                                                            |
| RESTORE       | MAY         | ASCII Text. Recovery instructions.                                                                                 | Should be understandable without external project state.                            |
| FORMAT-SPEC   | MAY         | ASCII Text. Copy of the relevant format specification.                                                             | Exact versioning rules are still open.                                              |
| CKSUM.B3SUM   | MAY         | ASCII Text. Checksums for selected bundle members. (produced by b3sum)                                             | BLAKE3 is preferred.                                                                |
| reader.tar.gz | MAY         | Gzipped Tarball. Minimal reader source package.                                                                    | Individual members may be compressed even though the top-level ar container is not. |
| medium-info   | MAY         | ASCII Text. Owner, organization, storage pool, handling notes, printable labels, or other non-authoritative hints. | Descriptive only; not required for restore correctness.                             |
| ...           | MAY         | Other files, can be cover pictures, whatever that fits.                                                            |                                                                                     |

## ar Subset Format

The Medium Header metadata bundle uses the restricted ar subset defined in
[docs/spec/00-header-common.md](00-header-common.md#ar-subset-format).

## Excluded Mutable State

The Medium Header MUST NOT record mutable media state, including:

- Archive instance list.
- Free space.
- Current used capacity.
- Last archive UUID.
- Last write timestamp.
- Any authoritative append position.

Archive discovery MUST be performed by scanning tape files for per-archive
Volume Headers and matching Archive End Headers.

## Security Rules

Readers MUST treat the ar metadata bundle as untrusted data.

Member names are identifiers for byte blobs, not restore paths. If extended ar
naming is supported, readers MUST reject absolute names and parent-directory
components.
