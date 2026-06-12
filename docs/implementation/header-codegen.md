# Python Header Codegen

Status: implementation note.

## Overview

NeoTape uses a Python codegen system to generate C++ header serialization,
parsing, and definition code from declarative Python data structures. The
generated code replaces mechanically repetitive hand-written code while keeping
non-mechanical logic (dispatch, CRC validation, UUID generation, timestamp
formatting) hand-written.

The Python definitions in `scripts/neotape_header_defs.py` are the **single
source of truth** for field ordering, sizes, types, and byte-level wire offsets.
The spec docs (`docs/spec/`) describe semantics and constraints; the Python defs
describe the exact binary layout.

## Scripts

### `scripts/neotape_header_defs.py`

Declares all three NeoTape fixed headers using three categories of definitions:

**Enums** — `HeaderType`, `PayloadProfile`, `FrameContentType` with their
underlying type and numeric values:

```python
ENUMS = {
    'HeaderType': {
        'underlying': 'uint8_t',
        'values': [
            ('volume', 1),
            ('frame', 2),
            ('archive_end', 3),
        ],
    },
    ...
}
```

**Flags** — bit position definitions with auto-generated `constexpr` test
helpers. Each flag group (e.g. `frame`, `archive_end`) specifies its storage
type and bit positions:

```python
FLAGS = {
    'frame': {
        'type': 'uint16_t',
        'bits': {'start': 0, 'end': 1},
    },
    'archive_end': {
        'type': 'uint16_t',
        'bits': {'clean_end': 0, 'catalog_present': 1},
    },
}
```

This generates C++ such as:

```cpp
inline constexpr uint16_t frame_flag_start = 1u << 0;
inline constexpr uint16_t frame_flag_end = 1u << 1;
inline constexpr bool has_frame_flag_start(uint16_t f) { return f & frame_flag_start; }
inline constexpr bool has_frame_flag_end(uint16_t f) { return f & frame_flag_end; }
```

**Header layouts** — each header type is a list of `Field` objects in wire
order. Offsets are computed by accumulating field sizes, exactly like
`__attribute__((packed))`. No offset values are hard-coded:

```python
HEADER_DEFS = {
    'VolumeHeader': {
        'type_enum': 'HeaderType::volume',
        'fields': [
            Field('magic', 8, 'magic', struct_member=False),
            Field('header_version', 1, 'const_uint8', const_value=1, struct_member=False),
            Field('header_type', 1, 'enum', enum_type='HeaderType', struct_member=False),
            Field('volume_block_size', 4, 'uint32', cxx_type='uint32_t'),
            ...
            Field('_reserved', 0, 'fill_to_1020'),
            Field('header_crc32c', 4, 'crc32c'),
        ],
    },
    ...
}
```

#### Field kinds

| Kind             | Struct member?                       | C++ type        | Put/get                                         |
| ---------------- | ------------------------------------ | --------------- | ----------------------------------------------- |
| `magic`        | No                                   | —              | Set by `make_header()`                        |
| `const_uint8`  | No                                   | —              | Set by `make_header()`                        |
| `enum`         | Yes (unless `struct_member=False`) | `enum_type`   | `static_cast<uint8_t>`                        |
| `uint8`        | Yes                                  | `uint8_t`     | Direct byte access                              |
| `uint16`       | Yes                                  | `uint16_t`    | `put_u16` / `get_u16`                       |
| `uint32`       | Yes                                  | `uint32_t`    | `put_u32` / `get_u32`                       |
| `uint64`       | Yes                                  | `uint64_t`    | `put_u64` / `get_u64`                       |
| `nt_uuid`      | Yes                                  | `std::string` | `put_fixed_string(size=37)`                   |
| `nt_name`      | Yes                                  | `std::string` | `put_fixed_string(size=256)`, `get_nt_name` |
| `nt_time`      | Yes                                  | `std::string` | `put_fixed_string(size=20)`                   |
| `nt_hash`      | Yes                                  | `Hash`        | `put_bytes` / `get_hash`                    |
| `ident64`      | Yes                                  | `std::string` | `put_fixed_string(size=64)`                   |
| `fill_to_1020` | No                                   | —              | Zero padding                                    |
| `crc32c`       | No                                   | —              | Computed by `finish_crc()`                    |

Fields with `struct_member=False` (prefix fields, padding, CRC) are handled by
the serialization machinery and never appear in the C++ struct definition.

#### Default values

Default values for struct members are derived from the field definition:

- Enums take the first value listed in `ENUMS` (e.g. `PayloadProfile::raw`)
- `Hash` fields get `Hash{}`
- String fields get empty string
- Numeric fields get `0`
- A `default` parameter on `Field` overrides this (e.g.
  `Field('flags', 2, 'uint16', ..., default='archive_end_flag_clean_end')`)

### `scripts/generate_neotape_parsers.py`

Reads `neotape_header_defs.py` and emits two files:

| File                                     | Contents                                                                                                                                                                        |
| ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `include/neotape/format_generated.hpp` | Enums, flag constants + test helpers, offset constants, struct definitions, inline put/get helpers,`make_header()`, `finish_crc()`, `check_common()`, parser declarations |
| `src/neotape_format_generated.cpp`     | Serializer function bodies, parser function bodies, name helper function bodies                                                                                                 |

#### Offset computation

The codegen computes offsets by walking the field list in order:

```python
offset = 0
for field in fields:
    if field.kind == 'fill_to_1020':
        field.size = 1020 - offset  # fill exactly to CRC position
    field.offset = offset
    offset += field.size
# CRC is always at offset 1020
```

This guarantees that generated offset constants always match the packed binary
layout. CRC32C is always at bytes 1020-1023 (the last 4 bytes of the 1024-byte
fixed header).

#### Offset constant naming

Offset constants use the existing prefix convention:

| Header type            | Prefix    |
| ---------------------- | --------- |
| VolumeHeader           | `vhdr_` |
| FrameHeader            | `fhdr_` |
| ArchiveEndHeader       | `ae_`   |
| Shared identity fields | `hdr_`  |

Shared identity fields (`volume_block_size`, `archive_uuid`, `archive_name`,
`volume_seq_num`, `payload_profile`) use the `hdr_` prefix to avoid
duplicates.

#### Serializer generation

Each header's serializer follows a consistent pattern:

```cpp
HeaderBytes serialize_volume_header(const VolumeHeader &h) {
    HeaderBytes bytes = make_header(HeaderType::volume);
    detail::put_u32(bytes, hdr_volume_block_size, h.volume_block_size);
    detail::put_fixed_string(bytes, hdr_archive_uuid, nt_uuid_size, h.archive_uuid);
    // ... one line per struct member ...
    finish_crc(bytes);
    return bytes;
}
```

#### Parser generation

Each header's parser mirrors the serializer:

```cpp
VolumeHeader parse_volume(const uint8_t *data) {
    VolumeHeader h;
    h.volume_block_size = detail::get_u32(data, hdr_volume_block_size);
    h.archive_uuid = detail::get_fixed_string(data, hdr_archive_uuid, nt_uuid_size);
    // ... one line per struct member ...
    return h;
}
```

## Generated files

### `format_generated.hpp`

This header is `#include`d by `format.hpp` and provides:

- `HeaderType`, `PayloadProfile`, `FrameContentType` enums
- Flag constants (`frame_flag_start`, `archive_end_flag_clean_end`, etc.)
- Flag test helpers (`has_frame_flag_start()`, `has_archive_end_flag_clean_end()`, etc.)
- All offset constants (`hdr_volume_block_size`, `fhdr_flags`, etc.)
- `VolumeHeader`, `FrameHeader`, `ArchiveEndHeader` structs
- `namespace detail` with put/get helpers (`put_u16`, `get_u64`, `put_fixed_string`, `get_hash`, etc.)
- `make_header()` — creates a `HeaderBytes` array with magic + version + type
- `finish_crc()` — computes and stores CRC32C at bytes 1020-1023
- `check_common()` — validates magic and header version
- Parser declarations (`parse_volume`, `parse_frame`, `parse_archive_end`)

### `format_generated.cpp`

This is compiled alongside the hand-written `neotape_format.cpp`. It provides:

- `serialize_volume_header()`, `serialize_frame_header()`, `serialize_archive_end_header()`
- `parse_volume()`, `parse_frame()`, `parse_archive_end()`
- `header_type_name()`, `payload_profile_name()`, `frame_content_type_name()`

## Hand-written files

### `include/neotape/format.hpp`

The hand-written header provides types and APIs that surround the generated
definitions:

- `HeaderBytes` (typedef for `std::array<uint8_t, 1024>`)
- `Hash` (typedef for `std::array<uint8_t, 32>`)
- `ParsedHeader` — variant-like struct holding a parsed header of any type
- Public function declarations for serializers, parsers, and utilities

It `#include`s `format_generated.hpp` for the generated types.

### `src/neotape_format.cpp`

Only non-mechanical logic:

- `parse_fixed_header()` — validates magic/version/CRC32C, dispatches to the
  generated `parse_volume`/`parse_frame`/`parse_archive_end`
- `blake3_hash()` — BLAKE3 computation wrapper
- `utc_timestamp_now()` — `%Y-%m-%dT%H:%M:%S` UTC timestamp
- `make_uuid_v4()` — RFC 4122 UUID generation
- `valid_block_size()` — range check

## Build integration

```makefile
GENERATOR     = scripts/generate_neotape_parsers.py
GENERATED_HPP = include/neotape/format_generated.hpp
GENERATED_CPP = src/neotape_format_generated.cpp

$(GENERATED_HPP) $(GENERATED_CPP): $(GENERATOR) scripts/neotape_header_defs.py
	python3 $(GENERATOR)

$(FORMAT_GEN_OBJ): $(GENERATED_CPP) $(GENERATED_HPP) Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $(GENERATED_CPP) -o $@
```

The generated object is linked into all tools that use format objects:

```makefile
$(BINDIR)/neotape-plan : ... $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) ...
```

Run manually:

```sh
python3 scripts/generate_neotape_parsers.py
```

Or via Make:

```sh
make generate
```

## Adding or modifying a header field

1. Edit the field list in `scripts/neotape_header_defs.py`. Add/remove/reorder
   `Field` entries following the wire order.
2. Run `python3 scripts/generate_neotape_parsers.py` to regenerate.
3. If a new field type is needed, add its kind to `size_arg()`, default
   handling, put/get dispatch, and the kind table in `is_struct_member()`.
4. Run `make` and verify.

No changes to hand-written C++ files are needed unless the struct layout change
affects the dispatch logic in `parse_fixed_header()` or `ParsedHeader`.

## Authoritative source of truth

The Python definitions in `scripts/neotape_header_defs.py` are the authoritative
reference for field ordering, sizes, and wire offsets. The spec docs
(`docs/spec/*.md`) describe field semantics and constraints. The deprecated
offset tables in `docs/implementation/phase-1-header-layout.md` are retained
only as a human-readable summary.
