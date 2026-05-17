# Header Common Rules

Status: draft / common rules.

This document defines rules shared by NeoTape fixed headers and trailers. Header
type-specific field inventories are defined in their own documents.

## Field Table Conventions

The exact binary layout, datatype mapping, and fixed field sizes are still being
defined. Field tables may include empty `datatype` and `size (in bytes)` columns
until those decisions are made explicitly.

## Multi-byte Datatypes

Unless explicitly stated, all multi-byte values (like int32) are little-endian.

## Magic Value

All NeoTape fixed headers and trailers MUST use the same 8-byte magic value:

```text
NeoTape\0
```

Header type is distinguished by each header or trailer's type field, not by a
different magic value.

## Common Header Prefix

Every NeoTape fixed header and trailer MUST begin with the same three fields in
this exact order:

| Field          | datatype | size (in bytes) |
| -------------- | -------- | --------------- |
| magic          | char[8]  | 8               |
| header_version | uint8    | 1               |
| header_type    | uint8    | 1               |

A parser can always read the first 10 bytes of any fixed header, validate the
magic value, determine the header layout version, and dispatch reader logic by
header type.

This common prefix applies to the Medium Header, Volume Header, Segment Header,
and Archive End Header. The layout after byte 9 is
type-specific.

## Header Position Rule

Every NeoTape fixed header MUST begin at byte 0 of its containing tape record.
No fixed header may start at a non-zero offset within a tape record.

## Fixed Header Size

Every NeoTape fixed header and trailer field area MUST occupy exactly 1024
bytes.

Every fixed header MUST place its CRC32C field as the last 4 bytes of that
1024-byte fixed area. The CRC32C is computed over all preceding fixed header
bytes, including empty values and reserved fields. The CRC32C field itself is
excluded from its own calculation.

Unused bytes in the 1024-byte fixed area MUST be represented as a reserved field
and MUST be written as zero by writers. Readers MUST include reserved bytes in
the CRC32C calculation.

## Data Continuation Rule

If a header type semantically defines continuation data that follows its fixed
field area (such as the Medium Header's ar metadata bundle or the Segment
Header's payload bytes), that continuation data MUST begin in the same tape
record immediately after the 1024-byte fixed field area, without padding or
alignment gap.

Header types that are not followed by continuation data (Volume Header, Archive
End Header) MAY fill the remainder of their tape record with padding. All other header types MUST NOT waste record space between the
fixed field area and their associated continuation data.

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
