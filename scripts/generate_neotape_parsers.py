#!/usr/bin/env python3
"""Generate C++ header parser/serializer code from neotape_header_defs.py.

Outputs:
  include/neotape/format_generated.hpp  — enums, flag helpers, offsets, structs, detail helpers
  src/neotape_format_generated.cpp      — serializers, parsers, name helpers, dispatch
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from neotape_header_defs import (
    ENUMS, FLAGS, HEADER_DEFS, compute_offsets,
    is_struct_member, put_fn, get_fn, size_arg, default_value, first_enum_value, Field,
)

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HPP_PATH = os.path.join(PROJECT_ROOT, 'include/neotape/format_generated.hpp')
CPP_PATH = os.path.join(PROJECT_ROOT, 'src/neotape_format_generated.cpp')

FIXED_HEADER_SIZE = 1024


# ── C++ template helpers ────────────────────────────────────────────

def cxx_guard(name: str) -> str:
    return f'NEOTAPE_{name.upper()}_HPP'

SHARED_IDENTITY = {'volume_block_size', 'archive_uuid', 'archive_name',
                   'volume_seq_num', 'payload_profile'}

def offset_constant_name(struct_name: str, field_name: str) -> str:
    """Generate a unique offset constant name from struct + field."""
    prefixes = {
        'VolumeHeader': 'vhdr',
        'FrameHeader': 'fhdr',
        'MediumHeader': 'mhdr',
        'ArchiveEndHeader': 'ae',
    }
    if field_name in SHARED_IDENTITY:
        return f'hdr_{field_name}'
    prefix = prefixes.get(struct_name, 'hdr')
    return f'{prefix}_{field_name}'

def pascal_to_snake(name: str) -> str:
    """Convert PascalCase to snake_case: VolumeHeader → volume_header"""
    result = []
    for i, ch in enumerate(name):
        if ch.isupper() and i > 0:
            result.append('_')
        result.append(ch.lower())
    return ''.join(result)

def serialize_func_name(struct_name: str) -> str:
    snake = pascal_to_snake(struct_name)
    return f'serialize_{snake}'

def parse_func_name(struct_name: str) -> str:
    snake = pascal_to_snake(struct_name)
    # Drop the _header suffix for parse functions to match existing API
    suffix = '_header'
    if snake.endswith(suffix):
        snake = snake[:-len(suffix)]
    return f'parse_{snake}'


# ── Generator ────────────────────────────────────────────────────────

def generate_hpp() -> str:
    lines = []
    def emit(s=''):
        lines.append(s)

    emit('// auto-generated — do not edit')
    emit('#pragma once')
    emit('#include <array>')
    emit('#include <cstddef>')
    emit('#include <cstdint>')
    emit('#include <string>')
    emit('#include <cstring>')
    emit('#include <algorithm>')
    emit('#include <stdexcept>')
    emit('#include <format>')
    emit('#include <crc32c/crc32c.h>')
    emit()
    emit('namespace neotape {')
    emit()

    # ── Fundamental constants ──
    emit(f'inline constexpr std::size_t fixed_header_size = {FIXED_HEADER_SIZE};')
    emit("inline constexpr std::array<char, 8> magic = {'N','e','o','T','a','p','e','\\0'};")
    emit('inline constexpr uint8_t header_version = 1;')
    emit('inline constexpr uint32_t min_block_size = 4096;')
    emit('inline constexpr uint32_t max_block_size = 8 * 1024 * 1024;')
    emit()
    emit('using HeaderBytes = std::array<uint8_t, fixed_header_size>;')
    emit('using Hash = std::array<uint8_t, 32>;')
    emit()
    emit('// Fixed string field sizes')
    emit('inline constexpr std::size_t nt_uuid_size = 37;')
    emit('inline constexpr std::size_t nt_name_size = 256;')
    emit('inline constexpr std::size_t nt_time_size = 20;')
    emit('inline constexpr std::size_t ident64_size = 64;')
    emit()

    # ── Enums ──
    for enum_name, info in ENUMS.items():
        emit(f'enum class {enum_name} : {info["underlying"]} {{')
        for val_name, val in info['values']:
            emit(f'    {val_name} = {val},')
        emit('};')
        emit()

    # ── Flag constants + helpers ──
    for group, info in FLAGS.items():
        for bit_name, bit_pos in info['bits'].items():
            constant_name = f'{group}_flag_{bit_name}'
            emit(f'inline constexpr {info["type"]} {constant_name} = 1u << {bit_pos};')
        emit()
        for bit_name, bit_pos in info['bits'].items():
            constant_name = f'{group}_flag_{bit_name}'
            helper_name = f'has_{constant_name}'
            emit(f'inline constexpr bool {helper_name}({info["type"]} f) {{ return f & {constant_name}; }}')
        emit()

    # ── Compute offsets for all headers ──
    header_offsets = {}  # struct_name -> [(field, offset)]
    for struct_name, hdef in HEADER_DEFS.items():
        header_offsets[struct_name] = compute_offsets(hdef)

    # ── Offset constants ──
    emit('// Common prefix offsets')
    emit('inline constexpr std::size_t com_header_version = 8;')
    emit('inline constexpr std::size_t com_header_type = 9;')
    emit()

    emit('// CRC32C always at the last 4 bytes of the fixed header')
    emit('inline constexpr std::size_t hdr_crc32c = 1020;')

    # Shared identity block offsets (from VolumeHeader layout)
    emit()
    emit('// Shared identity block offsets')
    shared_emitted = set()
    for f, off in header_offsets['VolumeHeader']:
        if f.name in SHARED_IDENTITY:
            cname = offset_constant_name('VolumeHeader', f.name)
            emit(f'inline constexpr std::size_t {cname} = {off};')
            shared_emitted.add(f.name)

    # Type-specific offsets
    emit()
    emit('// Type-specific offsets')
    for struct_name, hdef in HEADER_DEFS.items():
        emit(f'// {struct_name}')
        for f, off in header_offsets[struct_name]:
            if is_struct_member(f) and f.name not in SHARED_IDENTITY:
                cname = offset_constant_name(struct_name, f.name)
                emit(f'inline constexpr std::size_t {cname} = {off};')
    # ── Struct definitions ──
    for struct_name, hdef in HEADER_DEFS.items():
        emit(f'struct {struct_name} {{')
        for f in hdef['fields']:
            if not is_struct_member(f):
                continue
            if f.enum_type:
                cxx_type = f.enum_type
            elif f.cxx_type:
                cxx_type = f.cxx_type
            elif f.kind == 'nt_hash':
                cxx_type = 'Hash'
            else:
                cxx_type = f.cxx_type or 'uint8_t'
            if f.default:
                dv = f.default
            else:
                dv = default_value(f)
            emit(f'    {cxx_type} {f.name}{{{dv}}};')
        emit('};')
        emit()

    # ── Detail namespace: put/get helpers ──
    emit('namespace detail {')
    emit()
    emit('// ── Integer helpers ──')
    emit('inline void put_u16(HeaderBytes &bytes, std::size_t offset, uint16_t value) {')
    emit('    bytes[offset] = static_cast<uint8_t>(value & 0xffu);')
    emit('    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);')
    emit('}')
    emit()
    emit('inline void put_u32(HeaderBytes &bytes, std::size_t offset, uint32_t value) {')
    emit('    for (std::size_t i = 0; i < 4; ++i)')
    emit('        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);')
    emit('}')
    emit()
    emit('inline void put_u64(HeaderBytes &bytes, std::size_t offset, uint64_t value) {')
    emit('    for (std::size_t i = 0; i < 8; ++i)')
    emit('        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);')
    emit('}')
    emit()
    emit('inline uint16_t get_u16(const uint8_t *bytes, std::size_t offset) {')
    emit('    return static_cast<uint16_t>(bytes[offset]) |')
    emit('           static_cast<uint16_t>(bytes[offset + 1]) << 8;')
    emit('}')
    emit()
    emit('inline uint32_t get_u32(const uint8_t *bytes, std::size_t offset) {')
    emit('    uint32_t value = 0;')
    emit('    for (std::size_t i = 0; i < 4; ++i)')
    emit('        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);')
    emit('    return value;')
    emit('}')
    emit()
    emit('inline uint64_t get_u64(const uint8_t *bytes, std::size_t offset) {')
    emit('    uint64_t value = 0;')
    emit('    for (std::size_t i = 0; i < 8; ++i)')
    emit('        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);')
    emit('    return value;')
    emit('}')
    emit()
    emit('// ── Bytes / Hash helpers ──')
    emit('inline void put_bytes(HeaderBytes &bytes, std::size_t offset, const Hash &hash) {')
    emit('    std::ranges::copy(hash, bytes.begin() + static_cast<std::ptrdiff_t>(offset));')
    emit('}')
    emit()
    emit('inline Hash get_hash(const uint8_t *bytes, std::size_t offset) {')
    emit('    Hash hash{};')
    emit('    std::copy(bytes + offset, bytes + offset + hash.size(), hash.begin());')
    emit('    return hash;')
    emit('}')
    emit()
    emit('// ── String helpers ──')
    emit('inline void put_fixed_string(HeaderBytes &bytes, std::size_t offset, std::size_t size, std::string_view value) {')
    emit('    std::size_t n = std::min(value.size(), size - 1);')
    emit('    std::memcpy(bytes.data() + offset, value.data(), n);')
    emit('    std::memset(bytes.data() + offset + n, 0, size - n);')
    emit('}')
    emit()
    emit('inline std::string get_fixed_string(const uint8_t *bytes, std::size_t offset, std::size_t size) {')
    emit('    const auto *begin = reinterpret_cast<const char *>(bytes + offset);')
    emit('    const char *end = std::find(begin, begin + size, \'\\0\');')
    emit('    return std::string(begin, end);')
    emit('}')
    emit()
    emit('inline std::string get_nt_name(const uint8_t *bytes, std::size_t offset, std::size_t size) {')
    emit('    if (bytes[offset + size - 1] != 0)')
    emit('        throw std::runtime_error("nt_name field without trailing NUL");')
    emit('    return get_fixed_string(bytes, offset, size);')
    emit('}')
    emit()
    emit('} // namespace detail')
    emit()

    # ── make_header + finish_crc ──
    emit('// ── Header construction and CRC ──')
    emit('inline HeaderBytes make_header(HeaderType type) {')
    emit('    HeaderBytes bytes{};')
    emit('    for (std::size_t i = 0; i < magic.size(); ++i)')
    emit('        bytes[i] = static_cast<uint8_t>(magic[i]);')
    emit('    bytes[com_header_version] = header_version;')
    emit('    bytes[com_header_type] = static_cast<uint8_t>(type);')
    emit('    return bytes;')
    emit('}')
    emit()
    emit('inline void finish_crc(HeaderBytes &bytes) {')
    emit('    uint32_t crc = crc32c::Crc32c(bytes.data(), fixed_header_size - 4);')
    emit('    detail::put_u32(bytes, hdr_crc32c, crc);')
    emit('}')

    emit()
    emit('// ── Common prefix validation ──')
    emit('inline void check_common(const uint8_t *data, std::size_t size) {')
    emit('    if (size < fixed_header_size)')
    emit('        throw std::runtime_error("short fixed header");')
    emit('    for (std::size_t i = 0; i < magic.size(); ++i)')
    emit('        if (data[i] != static_cast<uint8_t>(magic[i]))')
    emit('            throw std::runtime_error("bad magic");')
    emit('    if (data[com_header_version] != header_version)')
    emit('        throw std::runtime_error(std::format("unsupported header version {}", data[8]));')
    emit('}')
    emit()
    emit()
    emit('// Parser declarations (implemented in generated .cpp)')
    for struct_name, hdef in HEADER_DEFS.items():
        pname = parse_func_name(struct_name)
        emit(f'{struct_name} {pname}(const uint8_t *data);')
    emit()
    emit('} // namespace neotape')
    return '\n'.join(lines)


def generate_cpp() -> str:
    lines = []
    def emit(s=''):
        lines.append(s)

    emit('// auto-generated — do not edit')
    emit('#include "neotape/format_generated.hpp"')
    emit('#include <crc32c/crc32c.h>')
    emit()
    emit('namespace neotape {')
    emit()

        # ── Serializer functions ──
    for struct_name, hdef in HEADER_DEFS.items():
        fields_with_offsets = compute_offsets(hdef)
        type_enum = hdef['type_enum']
        sname = serialize_func_name(struct_name)

        emit(f'HeaderBytes {sname}(const {struct_name} &h) {{')
        emit(f'    HeaderBytes bytes = make_header({type_enum});')

        for f, off in fields_with_offsets:
            if not is_struct_member(f):
                continue
            cname = offset_constant_name(struct_name, f.name)
            sz = size_arg(f.kind)
            if sz:
                emit(f'    detail::put_fixed_string(bytes, {cname}, {sz}, h.{f.name});')
            elif f.enum_type:
                emit(f'    bytes[{cname}] = static_cast<uint8_t>(h.{f.name});')
            elif f.kind == 'nt_hash':
                emit(f'    detail::put_bytes(bytes, {cname}, h.{f.name});')
            elif f.kind == 'uint8':
                emit(f'    bytes[{cname}] = h.{f.name};')
            else:
                put = {
                    'uint16': 'detail::put_u16',
                    'uint32': 'detail::put_u32',
                    'uint64': 'detail::put_u64',
                }.get(f.kind)
                if put:
                    emit(f'    {put}(bytes, {cname}, h.{f.name});')

        emit('    finish_crc(bytes);')
        emit('    return bytes;')
        emit('}')
        emit()

    # ── Parser functions ──
    for struct_name, hdef in HEADER_DEFS.items():
        fields_with_offsets = compute_offsets(hdef)
        pname = parse_func_name(struct_name)
        emit(f'{struct_name} {pname}(const uint8_t *data) {{');
        emit(f'    {struct_name} h;')
        for f, off in fields_with_offsets:
            if not is_struct_member(f):
                continue
            cname = offset_constant_name(struct_name, f.name)
            sz = size_arg(f.kind)
            if f.enum_type:
                emit(f'    h.{f.name} = static_cast<{f.enum_type}>(data[{cname}]);')
            elif f.kind == 'nt_hash':
                emit(f'    h.{f.name} = detail::get_hash(data, {cname});')
            elif f.kind == 'uint8':
                emit(f'    h.{f.name} = data[{cname}];')
            elif f.kind in ('uint16', 'uint32', 'uint64'):
                get = {'uint16': 'detail::get_u16', 'uint32': 'detail::get_u32', 'uint64': 'detail::get_u64'}
                emit(f'    h.{f.name} = {get[f.kind]}(data, {cname});')
            elif f.kind in ('nt_uuid', 'nt_time', 'ident64'):
                emit(f'    h.{f.name} = detail::get_fixed_string(data, {cname}, {sz});')
            elif f.kind == 'nt_name':
                emit(f'    h.{f.name} = detail::get_nt_name(data, {cname}, {sz});')
        emit('    return h;')
        emit('}')
        emit()

    # ── Name helpers ──
    for enum_name, info in ENUMS.items():
        # Build function name: header_type_name, payload_profile_name, frame_content_type_name
        if enum_name == 'HeaderType':
            func_name = 'header_type_name'
        elif enum_name == 'PayloadProfile':
            func_name = 'payload_profile_name'
        elif enum_name == 'FrameContentType':
            func_name = 'frame_content_type_name'
        else:
            func_name = f'{enum_name[0].lower()}{enum_name[1:]}_name'

        emit(f'std::string {func_name}({enum_name} type) {{')
        emit('    switch (type) {')
        for val_name, val in info['values']:
            emit(f'    case {enum_name}::{val_name}: return "{val_name}";')
        emit('    }')
        emit('    return "unknown";')
        emit('}')
        emit()

    emit('} // namespace neotape')
    return '\n'.join(lines)


# ── Main ────────────────────────────────────────────────────────────

def main():
    hpp = generate_hpp()
    cpp = generate_cpp()

    os.makedirs(os.path.dirname(HPP_PATH), exist_ok=True)
    os.makedirs(os.path.dirname(CPP_PATH), exist_ok=True)

    with open(HPP_PATH, 'w') as f:
        f.write(hpp)
    with open(CPP_PATH, 'w') as f:
        f.write(cpp)

    print(f'Generated {HPP_PATH}')
    print(f'Generated {CPP_PATH}')


if __name__ == '__main__':
    main()
