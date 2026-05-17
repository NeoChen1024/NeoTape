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
including empty values, padding, and reserved fields. The relevant CRC32C field
is excluded from its own calculation and MUST be treated as zero bytes during
that calculation.

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
