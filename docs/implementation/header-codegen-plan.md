# Python Codegen for NeoTape Headers

## Motivation

Replace hand-written offset constants, struct definitions, serializers, parsers,
and name helpers with mechanically generated C++ derived from a single Python
source of truth. Eliminates the risk of offset/size drift between spec tables
and implementation code.

## Scope

Generate into two files:
- `include/neotape/format_generated.hpp` — enums, flag constants + helpers,
  offset constants, struct definitions
- `src/neotape_format_generated.cpp` — serializer functions, parser functions,
  name helper functions

Keep hand-written (in `include/neotape/format.hpp` and
`src/neotape_format.cpp`):
- `HeaderBytes` / `Hash` typedefs
- `ParsedHeader` struct
- `parse_fixed_header()` dispatch function
- `blake3_hash()`, `utc_timestamp_now()`, `make_uuid_v4()`, `valid_block_size()`
- Low-level put/get helpers (`put_u16`, `get_u64`, `put_fixed_string`, etc.)

## Scripts

### `scripts/neotape_header_defs.py`

Single source of truth declaring every NeoTape binary header layout.

Three categories of definitions:

**Enums:**
```python
ENUMS = {
    'HeaderType': {
        'underlying': 'uint8_t',
        'values': [
            ('medium', 1),
            ('volume', 2),
            ('frame', 3),
            ('archive_end', 4),
        ],
    },
    'PayloadProfile': {
        'underlying': 'uint8_t',
        'values': [('raw', 1), ('pax', 2)],
    },
    'FrameContentType': {
        'underlying': 'uint8_t',
        'values': [('slice_content', 1), ('slice_metadata', 2)],
    },
}
```

**Flags:**
```python
FLAGS = {
    'frame': {
        'type': 'uint16_t',
        'bits': {
            'start': 0,
            'end':   1,
        },
    },
    'archive_end': {
        'type': 'uint16_t',
        'bits': {
            'clean_end':        0,
            'catalog_present':  1,
        },
    },
}
```

**Header layouts (no hard-coded offsets — codegen computes them by
accumulating field sizes, like `__attribute__((packed))`):**
```python
HEADER_DEFS = {
    'VolumeHeader': {
        'type_enum': 'HeaderType::volume',
        'fields': [
            Field(name='magic',            size=8,   kind='magic'),
            Field(name='header_version',   size=1,   kind='const_uint8', const_value=1),
            Field(name='header_type',      size=1,   kind='enum', enum_type='HeaderType'),
            Field(name='volume_block_size', size=4,  kind='uint32',  cxx_type='uint32_t'),
            Field(name='archive_uuid',     size=37,  kind='nt_uuid', cxx_type='std::string'),
            Field(name='archive_name',     size=256, kind='nt_name', cxx_type='std::string'),
            Field(name='volume_seq_num',   size=8,   kind='uint64',  cxx_type='uint64_t'),
            Field(name='payload_profile',  size=1,   kind='enum',    enum_type='PayloadProfile'),
            Field(name='volume_write_at_utc', size=20, kind='nt_time', cxx_type='std::string'),
            Field(name='flags',            size=2,   kind='uint16',  cxx_type='uint16_t'),
            Field(name='_reserved',        kind='fill_to_1020'),
            Field(name='header_crc32c',    size=4,   kind='crc32c'),
        ],
    },
    # FrameHeader, MediumHeader, ArchiveEndHeader follow the same pattern
}
```

The codegen assigns offsets sequentially:

```python
offset = 0
for field in fields:
    if field.kind == 'fill_to_1020':
        field.offset = offset
        field.size = 1020 - 4 - offset  # reserve 4 bytes for CRC32C
        break
    field.offset = offset
    offset += field.size

# CRC32C is always at offset 1020
```

This guarantees the generated C++ offset constants always match the packed
layout defined by the spec, eliminating drift entirely.

Each `Field` is a `dataclass`:
- `name` — field name (snake_case C++ identifier). Names starting with `_` are
  structural (padding/reserved) and excluded from the struct and from offset
  constant generation.
- `size` — encoded bytes (absent for `fill_to_1020` and `fill_to_1020_minus_crc`)
- `kind` — determines how it's read/written and whether it becomes a struct member:

| `kind` | Struct member? | Put/get |
|--------|---------------|---------|
| `magic` | No | Handled by `make_header` |
| `const_uint8` | No | Set at construction |
| `enum` | Yes, cast to enum | `static_cast` |
| `uint8`/`uint16`/`uint32`/`uint64` | Yes | `put_uXX` / `get_uXX` |
| `nt_uuid` | Yes, `std::string` | `put_fixed_string(size=37)` / `get_fixed_string` |
| `nt_name` | Yes, `std::string` | `put_fixed_string(size=256)` / `get_nt_name` |
| `nt_time` | Yes, `std::string` | `put_fixed_string(size=20)` / `get_fixed_string` |
| `nt_hash` | Yes, `Hash` | `put_bytes` / `get_hash` |
| `reserved` | No | Zero-filled |
| `crc32c` | No | Computed by `finish_crc` |

Field types ending in `_enum` in the spec (like `header_type`, `payload_profile`,
`frame_content_type`) use `kind='enum'` with the relevant `enum_type` name.

## Authoritative Source of Truth

After the codegen is operational, `scripts/neotape_header_defs.py` becomes the
authoritative reference for field ordering, sizes, and types — replacing the
offset tables in `docs/implementation/phase-1-header-layout.md`.

The spec docs (`docs/spec/00-format-common.md` through `04-archive-end-header.md`)
remain authoritative for field semantics, constraints, enum value meanings,
encoding rules, and requirement keywords. They should describe *what* each field
is and *why*, but they need not reproduce exact byte offsets.

The codegen Python defs are the single source of truth for *where* each field
lives on the wire — they are a machine-readable spec that happens to generate C++.
This means:
- `docs/spec/*.md` keeps describing semantics, datatypes, and constraints.
- `scripts/neotape_header_defs.py` keeps the exact field order and sizes.
- `docs/implementation/phase-1-header-layout.md` is deprecated for layout tables
  (the tables are now in the Python source).

### `scripts/generate_neotape_parsers.py`

Reads `neotape_header_defs.py`, emits C++ across four output sections:

**Section 1: enums + flag constants (`format_generated.hpp`)**
```cpp
// auto-generated
#pragma once
#include "neotape/format.hpp"

// Enums
enum class HeaderType : uint8_t { medium = 1, volume = 2, frame = 3, archive_end = 4 };
enum class PayloadProfile : uint8_t { raw = 1, pax = 2 };
enum class FrameContentType : uint8_t { slice_content = 1, slice_metadata = 2 };

// Flag constants
inline constexpr uint16_t frame_flag_start      = 1u << 0;
inline constexpr uint16_t frame_flag_end        = 1u << 1;
inline constexpr uint16_t archive_end_flag_clean_end       = 1u << 0;
inline constexpr uint16_t archive_end_flag_catalog_present = 1u << 1;

// Flag test helpers
constexpr bool has_frame_flag_start(uint16_t f)      { return f & frame_flag_start; }
constexpr bool has_frame_flag_end(uint16_t f)        { return f & frame_flag_end; }
constexpr bool has_archive_end_flag_clean_end(uint16_t f)       { return f & archive_end_flag_clean_end; }
constexpr bool has_archive_end_flag_catalog_present(uint16_t f) { return f & archive_end_flag_catalog_present; }

// Offset constants
inline constexpr std::size_t hdr_volume_block_size = 10;
inline constexpr std::size_t hdr_archive_uuid      = 14;
...
inline constexpr std::size_t vhdr_write_at_utc      = 316;
inline constexpr std::size_t vhdr_flags             = 336;

// Structs
struct VolumeHeader { ... };
struct FrameHeader { ... };
struct MediumHeader { ... };
struct ArchiveEndHeader { ... };
```

**Section 2: serializer functions (`format_generated.cpp`)**
```cpp
HeaderBytes serialize_volume_header(const VolumeHeader &header) {
    HeaderBytes bytes = make_header(HeaderType::volume);
    put_u32(bytes, hdr_volume_block_size, header.volume_block_size);
    put_fixed_string(bytes, hdr_archive_uuid, nt_uuid_size, header.archive_uuid);
    ...
    finish_crc(bytes);
    return bytes;
}
```

**Section 3: parser functions**
```cpp
VolumeHeader parse_volume(const uint8_t *data) {
    VolumeHeader header;
    header.volume_block_size = get_u32(data, hdr_volume_block_size);
    header.archive_uuid = get_fixed_string(data, hdr_archive_uuid, nt_uuid_size);
    ...
    return header;
}
```

**Section 4: name helpers (driven by enum definitions)**
```cpp
std::string header_type_name(HeaderType type) {
    switch (type) {
    case HeaderType::medium:     return "medium";
    case HeaderType::volume:     return "volume";
    case HeaderType::frame:      return "frame";
    case HeaderType::archive_end: return "archive_end";
    }
    return "unknown";
}
std::string payload_profile_name(PayloadProfile p) { ... }
std::string frame_content_type_name(FrameContentType t) { ... }
```

## Migration

| Step | Action |
|------|--------|
| 1 | Create `scripts/neotape_header_defs.py` with all enums, flags, and header layouts |
| 2 | Create `scripts/generate_neotape_parsers.py` |
| 3 | Run generator, inspect output |
| 4 | Trim `include/neotape/format.hpp`: remove old enum/flag/offset/struct definitions, add `#include "format_generated.hpp"` |
| 5 | Trim `src/neotape_format.cpp`: remove per-header serialize/parse/name-helper functions, keep dispatch + utilities |
| 6 | Add `src/neotape_format_generated.cpp` to Makefile with generator dependency |
| 7 | `make clean && make && bin/neotape-inspect ...` to verify round-trip |

## What Stays Hand-Written

```cpp
// format.hpp — kept
using HeaderBytes = std::array<uint8_t, 1024>;
using Hash = std::array<uint8_t, 32>;

struct ParsedHeader {
    HeaderType type;
    uint8_t version = 0;
    uint32_t stored_crc32c = 0;
    uint32_t computed_crc32c = 0;
    std::optional<VolumeHeader> volume;
    std::optional<FrameHeader> frame;
    std::optional<ArchiveEndHeader> archive_end;
    std::optional<MediumHeader> medium;
};

ParsedHeader parse_fixed_header(const uint8_t *data, std::size_t size);

// format.cpp — kept
// parse_fixed_header: check_common, CRC check, switch dispatch
// blake3_hash, utc_timestamp_now, make_uuid_v4, valid_block_size
```
