"""NeoTape header layout definitions — single source of truth for the on-wire format.

Field order and sizes are declared sequentially; the codegen computes offsets by
accumulating sizes, just like a packed C struct with __attribute__((packed)).
"""

from dataclasses import dataclass, field
from typing import Optional


# ── Field kinds ──────────────────────────────────────────────────────
#
# Each kind maps to a C++ read/write strategy and determines whether the
# field becomes a struct member.

@dataclass
class Field:
    name: str                 # snake_case identifier (leading _ ⇒ structural, excluded from struct)
    size: int = 0             # encoded bytes (0 for fill_to_1020)
    kind: str = ''            # see table below
    cxx_type: str = ''        # C++ type for struct member (empty = no member)
    enum_type: str = ''       # enum class name for 'enum' kind
    const_value: int = 0      # for 'const_uint8' kind
    default: str = ''         # default value expression (empty = zero-init)
    struct_member: bool = True # True → becomes a C++ struct field; False → skip

# Kind table:
#   magic         — 8-byte magic, handled by make_header(), no struct member
#   const_uint8   — fixed value (e.g. header_version=1), no struct member
#   enum          — uint8_t cast to enum_type, struct member
#   uint8         — uint8_t, struct member
#   uint16        — uint16_t (LE), struct member
#   uint32        — uint32_t (LE), struct member
#   uint64        — uint64_t (LE), struct member
#   nt_uuid       — 37-byte NUL-terminated UUID string, struct member (std::string)
#   nt_name       — 256-byte NUL-terminated+padded name, struct member (std::string)
#   nt_time       — 20-byte NUL-terminated timestamp, struct member (std::string)
#   nt_hash       — 32-byte BLAKE3 hash, struct member (Hash = std::array<uint8_t,32>)
#   fill_to_1020  — zero padding to fill to offset 1020, no struct member
#   crc32c        — 4-byte CRC32C at offset 1020, no struct member


# ── Enums ────────────────────────────────────────────────────────────

ENUMS = {
    'HeaderType': {
        'underlying': 'uint8_t',
        'values': [
            ('volume', 1),
            ('frame', 2),
            ('archive_end', 3),
        ],
    },
    'PayloadProfile': {
        'underlying': 'uint8_t',
        'values': [
            ('raw', 1),
            ('pax', 2),
        ],
    },
    'FrameContentType': {
        'underlying': 'uint8_t',
        'values': [
            ('slice_content', 1),
            ('slice_metadata', 2),
        ],
    },
}


# ── Flags ────────────────────────────────────────────────────────────

FLAGS = {
    'frame': {
        'type': 'uint16_t',
        'bits': {
            'start': 0,
            'end': 1,
        },
    },
    'archive_end': {
        'type': 'uint16_t',
        'bits': {
            'clean_end': 0,
            'catalog_present': 1,
        },
    },
}


# ── Header layouts ──────────────────────────────────────────────────
#
# Fields are listed in wire order. The codegen assigns offsets sequentially,
# so these MUST match the spec field tables exactly.

HEADER_DEFS = {
    'VolumeHeader': {
        'type_enum': 'HeaderType::volume',
        'fields': [
            Field('magic', 8, 'magic', struct_member=False),
            Field('header_version', 1, 'const_uint8', const_value=1, struct_member=False),
            Field('header_type', 1, 'enum', enum_type='HeaderType', struct_member=False),
            Field('volume_block_size', 4, 'uint32', cxx_type='uint32_t'),
            Field('archive_uuid', 37, 'nt_uuid', cxx_type='std::string'),
            Field('archive_name', 256, 'nt_name', cxx_type='std::string'),
            Field('volume_seq_num', 8, 'uint64', cxx_type='uint64_t'),
            Field('payload_profile', 1, 'enum', enum_type='PayloadProfile'),
            Field('volume_write_at_utc', 20, 'nt_time', cxx_type='std::string'),
            Field('flags', 2, 'uint16', cxx_type='uint16_t'),
            Field('_reserved', 0, 'fill_to_1020'),
            Field('header_crc32c', 4, 'crc32c'),
        ],
    },
    'FrameHeader': {
        'type_enum': 'HeaderType::frame',
        'fields': [
            Field('magic', 8, 'magic', struct_member=False),
            Field('header_version', 1, 'const_uint8', const_value=1, struct_member=False),
            Field('header_type', 1, 'enum', enum_type='HeaderType', struct_member=False),
            Field('volume_block_size', 4, 'uint32', cxx_type='uint32_t'),
            Field('archive_uuid', 37, 'nt_uuid', cxx_type='std::string'),
            Field('archive_name', 256, 'nt_name', cxx_type='std::string'),
            Field('volume_seq_num', 8, 'uint64', cxx_type='uint64_t'),
            Field('payload_profile', 1, 'enum', enum_type='PayloadProfile'),
            Field('logical_slice_seq_num', 8, 'uint64', cxx_type='uint64_t'),
            Field('global_frame_seq_num', 8, 'uint64', cxx_type='uint64_t'),
            Field('frame_seq_num_within_slice', 8, 'uint64', cxx_type='uint64_t'),
            Field('frame_payload_size', 8, 'uint64', cxx_type='uint64_t'),
            Field('frame_content_type', 1, 'enum', enum_type='FrameContentType'),
            Field('frame_payload_blake3', 32, 'nt_hash', cxx_type='Hash'),
            Field('flags', 2, 'uint16', cxx_type='uint16_t'),
            Field('slice_content_size', 8, 'uint64', cxx_type='uint64_t'),
            Field('slice_content_blake3', 32, 'nt_hash', cxx_type='Hash'),
            Field('_reserved', 0, 'fill_to_1020'),
            Field('header_crc32c', 4, 'crc32c'),
        ],
    },
    'ArchiveEndHeader': {
        'type_enum': 'HeaderType::archive_end',
        'fields': [
            Field('magic', 8, 'magic', struct_member=False),
            Field('header_version', 1, 'const_uint8', const_value=1, struct_member=False),
            Field('header_type', 1, 'enum', enum_type='HeaderType', struct_member=False),
            Field('volume_block_size', 4, 'uint32', cxx_type='uint32_t'),
            Field('archive_uuid', 37, 'nt_uuid', cxx_type='std::string'),
            Field('archive_name', 256, 'nt_name', cxx_type='std::string'),
            Field('volume_seq_num', 8, 'uint64', cxx_type='uint64_t'),
            Field('payload_profile', 1, 'enum', enum_type='PayloadProfile'),
            Field('last_logical_slice_seq_num', 8, 'uint64', cxx_type='uint64_t'),
            Field('last_global_frame_seq_num', 8, 'uint64', cxx_type='uint64_t'),
            Field('created_by_implementation', 64, 'ident64', cxx_type='std::string'),
            Field('created_by_build_id', 64, 'ident64', cxx_type='std::string'),
            Field('archive_end_at_utc', 20, 'nt_time', cxx_type='std::string'),
            Field('flags', 2, 'uint16', cxx_type='uint16_t', default='archive_end_flag_clean_end'),
            Field('_reserved', 0, 'fill_to_1020'),
            Field('header_crc32c', 4, 'crc32c'),
        ],
    },
}

# ── Offset computation ──────────────────────────────────────────────

def compute_offsets(header_def: dict) -> list[tuple[Field, int]]:
    """Assign offsets by accumulating field sizes. Returns [(field, offset), ...]."""
    result = []
    offset = 0
    for f in header_def['fields']:
        if f.kind == 'fill_to_1020':
            size = 1020 - offset
            f.size = size
        result.append((f, offset))
        offset += f.size
    return result


# ── Field kind traits ───────────────────────────────────────────────

def is_struct_member(field: Field) -> bool:
    if not field.struct_member:
        return False
    return field.kind not in ('magic', 'const_uint8', 'fill_to_1020', 'crc32c')

def put_fn(kind: str) -> str:
    return {
        'uint8': 'bytes[OFF] = static_cast<uint8_t>(V);',
        'uint16': 'detail::put_u16(bytes, OFF, V);',
        'uint32': 'detail::put_u32(bytes, OFF, V);',
        'uint64': 'detail::put_u64(bytes, OFF, V);',
        'enum': 'bytes[OFF] = static_cast<uint8_t>(V);',
        'nt_uuid': 'detail::put_fixed_string(bytes, OFF, SZ, V);',
        'nt_name': 'detail::put_fixed_string(bytes, OFF, SZ, V);',
        'nt_time': 'detail::put_fixed_string(bytes, OFF, SZ, V);',
        'nt_hash': 'detail::put_bytes(bytes, OFF, V);',
    }.get(kind, '')

def get_fn(kind: str) -> str:
    return {
        'uint8': 'static_cast<T>(data[OFF])',
        'uint16': 'detail::get_u16(data, OFF)',
        'uint32': 'detail::get_u32(data, OFF)',
        'uint64': 'detail::get_u64(data, OFF)',
        'enum': 'static_cast<T>(data[OFF])',
        'nt_uuid': 'detail::get_fixed_string(data, OFF, SZ)',
        'nt_name': 'detail::get_nt_name(data, OFF, SZ)',
        'nt_time': 'detail::get_fixed_string(data, OFF, SZ)',
        'nt_hash': 'detail::get_hash(data, OFF)',
    }.get(kind, '')

def size_arg(kind: str) -> str:
    return {
        'nt_uuid': 'nt_uuid_size',
        'nt_name': 'nt_name_size',
        'nt_time': 'nt_time_size',
        'ident64': 'ident64_size',
    }.get(kind, '')

def first_enum_value(enum_type: str) -> str:
    """Return the first value name for an enum type (used as default)."""
    for name, val in ENUMS.get(enum_type, {}).get('values', []):
        return name
    return ''

def default_value(field: Field) -> str:
    if field.const_value and field.kind == 'const_uint8':
        return str(field.const_value)
    if field.kind == 'nt_hash':
        return 'Hash{}'
    if field.cxx_type in ('std::string',):
        return ''
    if field.enum_type:
        first = first_enum_value(field.enum_type)
        if first:
            return f'{field.enum_type}::{first}'
        return '{}'.format(0)
    return '0'
