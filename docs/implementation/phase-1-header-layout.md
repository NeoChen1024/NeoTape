# Phase 1: Header Byte Layout

Status: implementation note.

## Overview

Phase 1 defined concrete byte offsets for all NeoTape fixed headers. The layout
is implemented in `include/neotape/format.hpp` as offset constants and struct
serializers in `src/neotape_format.cpp`.

## Design Decisions

### 1024-Byte Fixed Header

Every header type uses exactly 1024 bytes for its fixed field area. This is large
enough to accommodate repeated identity fields (UUID, name) plus type-specific
fields and reserved space, while small enough that a single 1024-byte write is
atomically safe on all target platforms.

### CRC32C in Last 4 Bytes

The CRC32C field is always at byte 1020 (the last 4 bytes of the 1024-byte
header). This allows a parser to read the full fixed header, compute CRC32C over
bytes 0–1019, and compare with bytes 1020–1023.

### Contiguous Packed Fields

All fields are packed contiguously with no alignment padding. This matches the
`__attribute__((packed))` C++ convention and ensures that byte-level offsets are
predictable from the spec tables alone.

### Common 10-Byte Prefix

```text
Offset 0:  magic        char[8]   "NeoTape\0"
Offset 8:  header_version uint8    1
Offset 9:  header_type    uint8_enum
```

Every header shares this prefix. A reader can always read 10 bytes, validate the
magic, and dispatch by `header_type`.

## Header Layout Tables (Deprecated)

> The active implementation now uses a handwritten unified Frame Header parser in
> `src/neotape_format.cpp`. The normative binary layout is `docs/spec/01-frame-header.md`.

## Serializer/Parser Design

The C++ implementation in `src/neotape_format.cpp` uses:

- **`serialize_header`** — packs field values into a `HeaderBytes` (1024-byte
  array) at the correct offsets, computes CRC32C, sets the final 4 bytes.
- **`parse_header`** — reads a `HeaderBytes`, validates magic, version, type,
  and CRC32C, then extracts type-specific fields by offset.

Both functions use `std::memcpy` with explicit offset constants for field
access, avoiding struct padding and endianness issues. Multi-byte integers are
stored in little-endian format.

## Open Items

- Frame Header repeated fields (`archive_uuid`, `archive_name`) — confirmed to
  stay for self-description. No volume-reference optimization planned.
- Reserved field allocation strategy — reserved bytes are zero for v0.1 and
  may be allocated by future versions.
