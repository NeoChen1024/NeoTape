# Unified Header Parser Design

## Context

NeoTape's normative format spec now defines one 512-byte unified Frame Header for every NeoTape record. The old implementation still has generated C++ for three fixed-header variants: `VolumeHeader`, `FrameHeader`, and `ArchiveEndHeader`, plus a dedicated volume-header record in the TCP writer flow.

This design replaces the generated multi-header model with a handwritten parser/serializer for the current unified header format in `docs/spec/`.

## Goals

- Replace generated header parser/serializer code with normal handwritten C++.
- Model only the unified 512-byte Frame Header from `docs/spec/01-frame-header.md`.
- Use the corrected header layout: 246 reserved bytes, a 72-byte signature field, and a final 32-byte `frame_hash`.
- Remove dedicated Volume Header records from TCP, tape, and spool output.
- Treat Archive End as a unified frame with `channel_type = ARCHIVE_END`, not as a separate header type.
- Keep fixed-header parsing separate from full-record hash verification.

## Non-Goals

- Do not add backward compatibility for the old 1024-byte generated header layout.
- Do not preserve old `VolumeHeader`, `ArchiveEndHeader`, `HeaderType`, `PayloadProfile`, or `FrameContentType` APIs.
- Do not implement Ed25519 signature verification in this parser refactor.
- Do not implement full restore semantics beyond updating existing read/write/spool behavior to parse the unified header.

## Public Format Model

`include/neotape/format.hpp` becomes the public source for format declarations. It no longer includes `neotape/format_generated.hpp`.

The core declarations are:

```cpp
inline constexpr std::size_t fixed_header_size = 512;
inline constexpr uint8_t header_version = 1;
inline constexpr uint32_t min_block_size = 4096;
inline constexpr uint32_t max_block_size = 8 * 1024 * 1024;

using HeaderBytes = std::array<uint8_t, fixed_header_size>;
using Hash = std::array<uint8_t, 32>;
using SignatureBytes = std::array<uint8_t, 72>;

enum class ChannelType : uint8_t {
    CH_CONTENT = 1,
    CH_METADATA = 2,
    ARCHIVE_END = 255,
};

inline constexpr uint64_t frame_flag_start = 1ull << 0;
inline constexpr uint64_t frame_flag_end = 1ull << 1;
inline constexpr uint64_t frame_flag_signed = 1ull << 2;
inline constexpr uint64_t frame_flag_clean_end = 1ull << 63;
```

The unified `FrameHeader` struct mirrors the spec fields:

- `ChannelType channel_type`
- `uint16_t volume_block_size_kib`
- `std::string archive_uuid`
- `std::string archive_label`
- `uint64_t volume_seq_num`
- `uint64_t global_frame_seq_num`
- `uint64_t logical_slice_seq_num`
- `uint64_t frame_seq_num_within_channel`
- `uint64_t frame_payload_size`
- `uint64_t flags`
- `SignatureBytes signature`
- `Hash frame_hash`

`signature` is exactly 72 bytes. Bytes 0-7 hold a 64-bit key ID and bytes 8-71 hold the raw 64-byte Ed25519 signature over `frame_hash`, matching the updated spec.

## Public Helpers

The format API will provide:

- `HeaderBytes serialize_frame_header(const FrameHeader &header)`
- `FrameHeader parse_frame_header(const uint8_t *data, std::size_t size)`
- `FrameHeader parse_fixed_header(const uint8_t *data, std::size_t size)` as the fixed-header parser used by existing call sites after this refactor
- `std::string channel_type_name(ChannelType type)`
- `std::string hash_hex(const Hash &hash)`
- `Hash blake3_hash(const uint8_t *data, std::size_t size)`
- `Hash compute_frame_hash(const uint8_t *data, std::size_t size)` for canonical full-record hash calculation
- `uint32_t decoded_block_size(const FrameHeader &header)` for `volume_block_size_kib * 1024`
- `bool valid_block_size(uint32_t block_size)` for decoded byte sizes

`channel_type_name()` returns spec strings such as `"ch_content"`, `"ch_metadata"`, and `"archive_end"` even though C++ enum values are uppercase.

## Header Layout

The handwritten parser uses explicit offsets from `docs/spec/01-frame-header.md`:

| Field | Offset | Size |
| --- | ---: | ---: |
| `magic` | 0 | 8 |
| `header_version` | 8 | 1 |
| `channel_type` | 9 | 1 |
| `volume_block_size_kib` | 10 | 2 |
| `archive_uuid` | 12 | 37 |
| `archive_label` | 49 | 65 |
| `volume_seq_num` | 114 | 8 |
| `global_frame_seq_num` | 122 | 8 |
| `logical_slice_seq_num` | 130 | 8 |
| `frame_seq_num_within_channel` | 138 | 8 |
| `frame_payload_size` | 146 | 8 |
| `flags` | 154 | 8 |
| `_reserved` | 162 | 246 |
| `signature` | 408 | 72 |
| `frame_hash` | 480 | 32 |

Delete the generated files `include/neotape/format_generated.hpp` and `src/neotape_format_generated.cpp`, and delete the obsolete codegen scripts `scripts/neotape_header_defs.py` and `scripts/generate_neotape_parsers.py`.

## Parser Validation

The fixed-header parser validates only invariants available from the fixed header:

- Input size is at least 512 bytes.
- Magic is exactly `NeoTape\0`.
- `header_version == 1`.
- `channel_type` is one of `CH_CONTENT`, `CH_METADATA`, or `ARCHIVE_END`.
- `volume_block_size_kib` decodes to a block size between 4 KiB and 8 MiB.
- `frame_payload_size <= decoded_block_size - fixed_header_size`.
- `_reserved` bytes are zero.
- Reserved flag bits 3-62 are zero.
- `archive_uuid` and `archive_label` are NUL-terminated within their fixed fields.

For `ARCHIVE_END`, the parser also validates:

- `START`, `END`, and `CLEAN_END` are set.
- `SIGNED` is allowed but not verified by this refactor.
- `logical_slice_seq_num == 0`.
- `frame_seq_num_within_channel == 1`.

For `CH_CONTENT` and `CH_METADATA`, the parser rejects `CLEAN_END` because the spec says `CLEAN_END` is only valid for `archive_end`.

The parser extracts `signature` and `frame_hash` but does not verify the BLAKE3 frame hash because fixed-header parsing may not have the full record. It also does not reject a non-zero unsigned `signature` field; the updated common rules say readers ignore the signature field when `SIGNED` is clear.

## Frame Hash

Full-record hash verification uses a separate helper, not the fixed-header parser.

`compute_frame_hash()` follows `docs/spec/00-format-common.md`:

- Hash exactly `volume_block_size_kib * 1024` bytes.
- Treat bytes 408-479, the 72-byte `signature` field, as zero.
- Treat bytes 480-511, the 32-byte `frame_hash` field, as zero.
- Include all other header bytes, payload bytes, and padding bytes exactly as stored.

Writers build a full zero-padded record, serialize the header with an empty `frame_hash`, compute the canonical hash, and write the resulting hash to bytes 480-511. If `SIGNED` is set in future work, the writer signs the hash and writes the 8-byte key ID plus 64-byte Ed25519 signature into bytes 408-479 before final output; the signature bytes remain zeroed for hash calculation.

## TCP And Tape Flow

The archiver/writer protocol stops exchanging a separate volume header:

- Remove `get_volume_header` and `volume_header` message types and handling.
- The writer starts by sending `next_frame`.
- The first returned `frame_record` is the first NeoTape record on the volume.
- The writer parses that first frame to discover the decoded block size.
- Archive end is sent as a normal `frame_record` with `channel_type = ARCHIVE_END`.
- Remove the separate `archive_end_header` message type once archive end is represented as a frame record.

This keeps the TCP payload stream aligned with the tape layout: every NeoTape payload delivered to the writer is a complete unified frame record.

## Producer Behavior

`neotape-archiver` builds records with the unified `FrameHeader`:

- Content frames use `ChannelType::CH_CONTENT`.
- The current implementation can continue using one content frame per logical slice as an implementation simplification.
- `frame_seq_num_within_channel` replaces the old `frame_seq_num_within_slice` naming.
- `volume_block_size_kib` stores KiB, not bytes.
- `archive_label` replaces the old `archive_name` header field.
- Archive end uses `ChannelType::ARCHIVE_END`, normally zero payload, `logical_slice_seq_num = 0`, `frame_seq_num_within_channel = 1`, and `START | END | CLEAN_END`.

## Consumer Behavior

`neotape-write` no longer writes an initial volume-header record. It writes each received frame record as-is, including the final archive-end frame. End-of-tape handling still acknowledges the last fully written global frame sequence number.

`neotape-read` and spool handling parse unified headers:

- `CH_CONTENT` and `CH_METADATA` records are slice records.
- `ARCHIVE_END` marks archive completion.
- Spool filenames use the logical slice sequence for content/metadata frames and `archive-end` for archive end.
- The reader's success check changes from "saw volume header and archive end header" to "saw at least one content/metadata frame and archive end".

## Build And Documentation Changes

The Makefile removes the codegen target and generated object:

- Remove `FORMAT_GEN_OBJ`.
- Remove `GENERATOR`, `GENERATED_HPP`, and `GENERATED_CPP` variables and rules.
- Link tools only with `src/neotape_format.o` for format parsing/serialization.
- Include the now-handwritten format files in `make format` and `make tidy` normally.

Update `AGENTS.md` and stale implementation docs that say Python codegen is the source of truth. Those statements are no longer correct after this refactor.

## Tests

Update smoke tests for the unified format:

- `HEADER_SIZE=512`.
- No volume-header record is expected.
- The first record's byte 9 is `0x01` for `CH_CONTENT`.
- The archive-end record's byte 9 is `0xff`.
- Size expectations account for no separate volume-header record.
- Multi-volume smoke tests verify both volumes begin with normal frame records, not volume headers.

Run at least:

```sh
make -j "$(nproc)"
make test
```

After deleting generated source files, regenerate the compilation database:

```sh
make compile_commands
```

## Risks

- This is a format-breaking change for any data produced by the old generated 1024-byte headers. The project should intentionally reject old data unless backward compatibility is explicitly requested later.
- Removing volume-header and archive-end-header TCP messages changes the client/server protocol. Existing old clients and servers will not interoperate.
- Hash verification depends on full-record access; call sites that parse only the fixed header must not claim they verified frame integrity.
- Signature authentication remains unimplemented in this refactor even though the fixed header now reserves the 72-byte Ed25519 payload field.
