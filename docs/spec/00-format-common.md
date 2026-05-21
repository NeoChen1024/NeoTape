# Common Format Rules

Status: draft / common rules.

This document defines common rules and conventions shared across the NeoTape
format. Header type-specific field inventories are defined in their own
documents.

## Datatype Reference

All fixed header field tables in this specification use the following datatypes:

| Datatype       | Description                                                                                                                                                                          |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `uint8`      | Unsigned 8-bit integer.                                                                                                                                                              |
| `uint8_enum` | Unsigned 8-bit integer with enumerated values defined per field.                                                                                                                     |
| `uint16`     | Unsigned 16-bit integer, little-endian.                                                                                                                                              |
| `uint32`     | Unsigned 32-bit integer, little-endian.                                                                                                                                              |
| `uint64`     | Unsigned 64-bit integer, little-endian.                                                                                                                                              |
| `char[N]`    | Fixed N-byte character array. NUL-terminated or NUL-padded per field rules.                                                                                                          |
| `byte[N]`    | Fixed N-byte raw binary array.                                                                                                                                                       |
| `byte[*]`    | Auto-calculated zero padding. Expands to the number of zero bytes needed to fill the 1024-byte fixed header area. Writers MUST write zero; readers MUST include all bytes in CRC32C. |
| `nt_name`    | Fixed 256-byte UTF-8 text field. NUL-terminated, NUL-padded after the terminator, required to end with NUL. (Max 255 bytes usable)                                                   |
| `nt_time`    | NUL-terminated UTC timestamp. 20 bytes. Encoding defined in Timestamp Format.                                                                                                        |
| `nt_uuid`    | NUL-terminated UUID string per RFC 4122. 37 bytes.                                                                                                                                   |
| `nt_hash`    | BLAKE3 256-bit hash. 32 bytes.                                                                                                                                                       |
| `nt_crc32c`  | CRC32C checksum, little-endian. 4 bytes. Algorithm defined in CRC32C Algorithm section.                                                                                              |

## Magic Value

All NeoTape fixed headers MUST use the same 8-byte magic value:

```text
NeoTape\0
```

Header type is distinguished by each header's type field, not by a different
magic value.

## Field Packing

All fields in every NeoTape fixed header MUST be packed contiguously — no
padding, alignment, or gap bytes between fields. This is equivalent to a C or
C++ packed struct (`__attribute__((packed))`).

Field sizes in the tables are exact and cumulative: the byte offset of a field
equals the sum of all preceding field sizes in that header's table.

## Common Header Prefix

Every NeoTape fixed header MUST begin with the same three fields in this exact
order:

| Field          | datatype   | size (in bytes) |
| -------------- | ---------- | --------------- |
| magic          | char[8]    | 8               |
| header_version | uint8      | 1               |
| header_type    | uint8_enum | 1               |

A parser can always read the first 10 bytes of any fixed header, validate the
magic value, determine the header layout version, and dispatch reader logic by
header type.

This common prefix applies to the Medium Header, Volume Header, Frame Header,
and Archive End Header. The layout after byte 9 is type-specific.

## Repeated Archive Identity Fields

Archive-time headers SHOULD repeat enough identity fields to make an archive
recognizable during low-level inspection, even without a NeoTape-aware tool.

Volume Header, Frame Header, and Archive End Header therefore include
`archive_uuid` and `archive_name`. `archive_uuid` is the authoritative machine
identifier. `archive_name` is a human-readable hint and MUST NOT be used as a
unique key.

## Header Position Rule

Every NeoTape fixed header MUST begin at byte 0 of its containing NeoTape
record. No fixed header may start at a non-zero offset within a record.

## Fixed Header Size

Every NeoTape fixed header field area MUST occupy exactly 1024 bytes.

Every fixed header MUST place its CRC32C field as the last 4 bytes of that
1024-byte fixed area. The CRC32C is computed over all preceding fixed header
bytes, including empty values and reserved fields. The CRC32C field itself is
excluded from its own calculation.

Unused bytes in the 1024-byte fixed area MUST be represented as a reserved field
and MUST be written as zero by writers. Readers MUST include reserved bytes in
the CRC32C calculation.

## CRC32C Algorithm

NeoTape uses CRC32C (Castagnoli polynomial) with the following parameters:

- Polynomial: `0x82F63B78`
- Initial value: `0xFFFFFFFF`
- Final XOR: `0xFFFFFFFF`
- Input reflection: yes
- Result reflection: yes

Implementations MAY use any compatible method (lookup-table, slicing-by-8,
hardware CRC32C instructions).

The CRC32C is computed over all fixed header bytes preceding the CRC32C field,
including reserved and zero-filled fields, in their on-media byte order. The
CRC32C field itself is excluded from the computation.

All `nt_crc32c` fields in NeoTape headers use this algorithm.

## Data Continuation Rule

If a header type semantically defines continuation data that follows its fixed
field area, that continuation data MUST begin in the same NeoTape record
immediately after the 1024-byte fixed field area, without padding or alignment
gap.

Examples include the Medium Header's ar metadata bundle and the Frame Header's
content or metadata bytes.

Header types that are not followed by continuation data (Volume Header, Archive
End Header) MAY fill the remainder of their NeoTape record with padding. All
other header types MUST NOT waste record space between the fixed field area and
their associated continuation data.

## Block Size Constraints

### Minimum

The NeoTape record block size MUST be at least 4 KiB (4096 bytes).

A writer SHOULD use at least 64 KiB (65536 bytes) in practice. Below 64 KiB
the Frame Header overhead (1024 bytes per record) becomes significant: at 4 KiB
the header consumes 25% of each record.

### Maximum

The NeoTape record block size MUST NOT exceed 8 MiB (8388608 bytes). A reader
SHOULD reject larger values as unsupported.

8 MiB is the practical variable-length record ceiling validated on the target
LTO-5 drive. Values above 8 MiB may fail at the hardware layer.

### Shape

The format MAY use non-power-of-2 block sizes. In practice, that is usually a
poor choice for any physical medium, especially LTO tape. Writers SHOULD prefer
power-of-2 block sizes unless they have a concrete medium-specific reason not
to.

### Scope

These constraints apply to both `volume_block_size` (Volume Header, Frame Header,
Archive End Header) and `medium_header_block_size` (Medium Header).

## Requirement Keywords

For fixed header field tables, `MUST`, `SHOULD`, and `MAY` describe whether a
writer is required or expected to produce a meaningful value. They do not make
the field itself optional.

Every fixed field listed in a header field table has a stable position and fixed
encoded size. Writers MUST NOT omit fields from the encoded header. If a writer
does not produce a meaningful value for a `SHOULD` or `MAY` field, it MUST write
the field's empty value instead.

Empty fixed-field values are encoded as follows:

- Numeric fields: zero.
- Fixed byte arrays: all zero bytes.
- NUL-terminated string fields: first byte NUL, remaining bytes zero.
- `nt_name` fields: first byte NUL, remaining bytes zero.

CRC32C calculations over fixed fields MUST include every fixed field byte,
including empty values and reserved fields. The relevant CRC32C field is
excluded from its own calculation.

This rule applies to fixed header fields only. Metadata bundle or catalog member
tables are file lists; `MAY` member files may be absent from the relevant
container.

## Timestamp Format

All fixed NeoTape timestamp fields MUST use UTC and MUST be encoded as a
20-byte NUL-terminated string.

The timestamp text before the NUL byte MUST match this exact `strftime` format:

```text
%Y-%m-%dT%H:%M:%S
```

This is exactly 19 ASCII bytes followed by one NUL byte:

```text
YYYY-MM-DDTHH:MM:SS\0
```

Writers MUST NOT use timezone suffixes, numeric offsets, fractional seconds,
locale-specific text, RFC 3339 variants, ISO 8601 variants, or any other date
format.

## ar Subset Format

NeoTape metadata bundles use a restricted SVR4/GNU ar subset:

- Global magic: `!<arch>\n`.
- Thin archive magic is not allowed: `!<thin>\n`.
- No symbol table member: member name `/` is not allowed.
- No long-name table: member name `//` is not allowed.
- No path-like member names: member names MUST NOT contain `/`, `\`, NUL, or newline.
- Member names MUST be ASCII and MUST fit directly in the fixed 16-byte ar name field.
- Writer SHOULD use the GNU/SVR4 short-name convention: `name/` followed by spaces.
- With that convention, the portable member name limit is 15 bytes before the trailing `/`.
- Header fields are ASCII and space padded.
- `mtime`, `uid`, `gid`, and `size` fields are ASCII decimal.
- `mode` is ASCII octal.
- Member size is stored in the standard 10-byte ar size field and counts only member data bytes.
- Header trailer magic MUST be `` `\n ``.
- If member data size is odd, the writer MUST append one `\n` padding byte after the member data.
- The odd-size padding byte is not counted in the member size field.
