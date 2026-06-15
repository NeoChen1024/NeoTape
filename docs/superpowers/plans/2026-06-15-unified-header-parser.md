# Unified Header Parser Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the generated three-header format layer with a handwritten unified 512-byte Frame Header parser/serializer and align TCP/tape/spool behavior with `docs/spec/`.

**Architecture:** `include/neotape/format.hpp` and `src/neotape_format.cpp` become the single format implementation. TCP moves to a stream of unified `frame_record` messages; archive end is a frame with `ChannelType::ARCHIVE_END`, and there is no volume-header exchange or tape record.

**Tech Stack:** C++20, GNU Make, BLAKE3 C library, existing shell smoke tests, no external test framework.

**Repository rule:** Do not commit during execution unless the user explicitly asks for commits. Use `git status --short` and `git diff` as checkpoints instead.

---

## File Structure

- Modify `include/neotape/format.hpp`: define the unified header API, uppercase `ChannelType` enum values, flag constants, `FrameHeader`, and format helpers.
- Modify `src/neotape_format.cpp`: implement layout offsets, serializer, parser validation, canonical frame-hash calculation, BLAKE3, UUID, timestamp, and block-size helpers.
- Delete `include/neotape/format_generated.hpp`: generated header is obsolete.
- Delete `src/neotape_format_generated.cpp`: generated implementation is obsolete.
- Delete `scripts/neotape_header_defs.py`: old Python layout source is obsolete.
- Delete `scripts/generate_neotape_parsers.py`: old generator is obsolete.
- Modify `Makefile`: remove format codegen targets and add `bin/test_format`.
- Modify `include/neotape/tcp_protocol.hpp`: remove volume-header and archive-end-header message types.
- Modify `src/neotape_tcp_protocol.cpp`: remove names for deleted message types.
- Modify `tests/test_tcp_protocol.cpp`: round-trip only the remaining message types.
- Create `tests/test_format.cpp`: unit-style executable for unified header layout, validation, and canonical hash behavior.
- Modify `src/neotape_tcp_server.cpp`: build unified records, remove volume-header serving, and send archive end as a normal frame record.
- Modify `src/neotape_write_cmd.cpp`: request frames immediately, discover block size from first frame, and write archive end as a normal record.
- Modify `src/neotape_read_cmd.cpp`: parse unified headers and stop checking for volume headers.
- Modify `src/neotape_tape.cpp`: name/finalize spool files from unified `FrameHeader` values.
- Modify `tests/lto-variable-block-record-probe.cpp`: update the probe to write/read unified frames and remove stale `tape_navigator` usage.
- Modify `tests/smoke_tcp_archive.sh`: update header size, channel checks, and size expectations.
- Modify `tests/smoke_tcp_archive_multi.sh`: update header size and first-record checks.
- Modify `AGENTS.md`: remove Python codegen guidance.
- Delete `docs/implementation/header-codegen.md`: obsolete implementation note.
- Delete `docs/implementation/header-codegen-plan.md`: obsolete plan.
- Modify `docs/implementation/phase-1-header-layout.md`: remove the note that codegen is the source of truth.
- Regenerate `compile_commands.json` after source deletion with `make compile_commands`.

## Task 1: Add Unified Format Tests First

**Files:**
- Create: `tests/test_format.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Add `tests/test_format.cpp`**

Create `tests/test_format.cpp` with this content:

```cpp
#include "neotape/format.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "test_format: " << msg << "\n";
    std::exit(1);
}

void expect(bool ok, const std::string &msg) {
    if (!ok)
        fail(msg);
}

template <class Fn>
void expect_throw(Fn fn, const std::string &msg) {
    try {
        fn();
    } catch (const std::exception &) {
        return;
    }
    fail(msg);
}

uint16_t le16(const neotape::HeaderBytes &b, std::size_t off) {
    return static_cast<uint16_t>(b[off]) |
           static_cast<uint16_t>(b[off + 1]) << 8;
}

uint64_t le64(const neotape::HeaderBytes &b, std::size_t off) {
    uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    return v;
}

neotape::FrameHeader make_content_header() {
    neotape::FrameHeader h;
    h.channel_type = neotape::ChannelType::CH_CONTENT;
    h.volume_block_size_kib = 4;
    h.archive_uuid = "00000000-0000-4000-8000-000000000123";
    h.archive_label = "unit-test";
    h.volume_seq_num = 7;
    h.global_frame_seq_num = 9;
    h.logical_slice_seq_num = 2;
    h.frame_seq_num_within_channel = 1;
    h.frame_payload_size = 123;
    h.flags = neotape::frame_flag_start | neotape::frame_flag_end;
    h.signature[0] = 0xaa;
    h.signature[71] = 0xbb;
    h.frame_hash[0] = 0xcc;
    h.frame_hash[31] = 0xdd;
    return h;
}

void test_layout_round_trip() {
    neotape::FrameHeader h = make_content_header();
    neotape::HeaderBytes b = neotape::serialize_frame_header(h);

    expect(b.size() == 512, "header size should be 512");
    expect(std::memcmp(b.data(), "NeoTape\0", 8) == 0, "bad magic");
    expect(b[8] == 1, "bad header version");
    expect(b[9] == 1, "bad channel type");
    expect(le16(b, 10) == 4, "bad volume_block_size_kib");
    expect(le64(b, 114) == 7, "bad volume_seq_num");
    expect(le64(b, 122) == 9, "bad global_frame_seq_num");
    expect(le64(b, 130) == 2, "bad logical_slice_seq_num");
    expect(le64(b, 138) == 1, "bad frame_seq_num_within_channel");
    expect(le64(b, 146) == 123, "bad frame_payload_size");
    expect(le64(b, 154) == (neotape::frame_flag_start | neotape::frame_flag_end),
           "bad flags");
    expect(b[408] == 0xaa, "bad signature start offset");
    expect(b[479] == 0xbb, "bad signature end offset");
    expect(b[480] == 0xcc, "bad frame_hash start offset");
    expect(b[511] == 0xdd, "bad frame_hash end offset");

    neotape::FrameHeader parsed = neotape::parse_fixed_header(b.data(), b.size());
    expect(parsed.channel_type == neotape::ChannelType::CH_CONTENT, "parsed channel mismatch");
    expect(parsed.volume_block_size_kib == 4, "parsed block size mismatch");
    expect(parsed.archive_uuid == h.archive_uuid, "parsed uuid mismatch");
    expect(parsed.archive_label == h.archive_label, "parsed label mismatch");
    expect(parsed.global_frame_seq_num == 9, "parsed global seq mismatch");
    expect(parsed.frame_payload_size == 123, "parsed payload size mismatch");
    expect(parsed.signature[0] == 0xaa && parsed.signature[71] == 0xbb,
           "parsed signature mismatch");
    expect(parsed.frame_hash[0] == 0xcc && parsed.frame_hash[31] == 0xdd,
           "parsed hash mismatch");
    expect(neotape::decoded_block_size(parsed) == 4096, "decoded block size mismatch");
}

void test_validation() {
    neotape::FrameHeader h = make_content_header();
    neotape::HeaderBytes b = neotape::serialize_frame_header(h);

    auto reserved = b;
    reserved[162] = 1;
    expect_throw([&] { neotape::parse_fixed_header(reserved.data(), reserved.size()); },
                 "reserved byte should be rejected");

    auto reserved_flag = b;
    reserved_flag[154] = static_cast<uint8_t>(reserved_flag[154] | 0x08u);
    expect_throw([&] { neotape::parse_fixed_header(reserved_flag.data(), reserved_flag.size()); },
                 "reserved flag bit should be rejected");

    auto content_clean_end = neotape::serialize_frame_header(make_content_header());
    content_clean_end[161] = static_cast<uint8_t>(content_clean_end[161] | 0x80u);
    expect_throw([&] { neotape::parse_fixed_header(content_clean_end.data(),
                                                   content_clean_end.size()); },
                 "CLEAN_END on content frame should be rejected");

    neotape::FrameHeader ae;
    ae.channel_type = neotape::ChannelType::ARCHIVE_END;
    ae.volume_block_size_kib = 4;
    ae.archive_uuid = "00000000-0000-4000-8000-000000000123";
    ae.archive_label = "unit-test";
    ae.volume_seq_num = 1;
    ae.global_frame_seq_num = 10;
    ae.logical_slice_seq_num = 0;
    ae.frame_seq_num_within_channel = 1;
    ae.frame_payload_size = 0;
    ae.flags = neotape::frame_flag_start | neotape::frame_flag_end |
               neotape::frame_flag_clean_end;
    b = neotape::serialize_frame_header(ae);
    neotape::FrameHeader parsed = neotape::parse_fixed_header(b.data(), b.size());
    expect(parsed.channel_type == neotape::ChannelType::ARCHIVE_END,
           "archive end should parse");

    b[161] = static_cast<uint8_t>(b[161] & 0x7fu);
    expect_throw([&] { neotape::parse_fixed_header(b.data(), b.size()); },
                 "archive end without CLEAN_END should be rejected");
}

void test_frame_hash_canonicalization() {
    neotape::FrameHeader h = make_content_header();
    neotape::HeaderBytes header = neotape::serialize_frame_header(h);

    std::vector<uint8_t> record(4096, 0);
    std::copy(header.begin(), header.end(), record.begin());
    record[512] = 0x42;
    record[513] = 0x43;

    neotape::Hash hash = neotape::compute_frame_hash(record.data(), record.size());

    std::vector<uint8_t> canonical = record;
    std::fill(canonical.begin() + 408, canonical.begin() + 480, 0);
    std::fill(canonical.begin() + 480, canonical.begin() + 512, 0);
    expect(hash == neotape::blake3_hash(canonical.data(), canonical.size()),
           "canonical hash mismatch");

    std::vector<uint8_t> changed_sig_and_hash = record;
    changed_sig_and_hash[408] ^= 0xff;
    changed_sig_and_hash[480] ^= 0xff;
    expect(hash == neotape::compute_frame_hash(changed_sig_and_hash.data(),
                                               changed_sig_and_hash.size()),
           "signature/hash bytes must be ignored by canonical hash");

    std::vector<uint8_t> changed_payload = record;
    changed_payload[512] ^= 0xff;
    expect(hash != neotape::compute_frame_hash(changed_payload.data(),
                                               changed_payload.size()),
           "payload changes must affect canonical hash");
}

} // namespace

int main() {
    test_layout_round_trip();
    test_validation();
    test_frame_hash_canonicalization();
    std::cout << "test_format: ok\n";
    return 0;
}
```

- [ ] **Step 2: Add the test target to `Makefile`**

Change the `EXE` line to include `bin/test_format`:

```make
EXE	= bin/mt-pax bin/neotape-plan bin/test_pax_pipeline bin/test_tcp_protocol bin/test_format bin/neotape-archiver bin/neotape-write bin/neotape-read
```

Add this target after `bin/test_tcp_protocol`:

```make
$(BINDIR)/test_format : tests/test_format.cpp $(FORMAT_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) -o $@ $(LDLIBS)
```

Change the `test` target prerequisites and commands:

```make
test: $(BINDIR)/test_pax_pipeline $(BINDIR)/test_tcp_protocol $(BINDIR)/test_format $(BINDIR)/mt-pax $(BINDIR)/neotape-plan $(BINDIR)/neotape-archiver $(BINDIR)/neotape-write $(BINDIR)/neotape-read
	$(BINDIR)/test_pax_pipeline
	$(BINDIR)/test_tcp_protocol
	$(BINDIR)/test_format
	sh tests/smoke_mt_pax_pipeline.sh
	sh tests/smoke_tcp_archive.sh
	sh tests/smoke_tcp_archive_multi.sh
	sh tests/smoke_mt_pax_parity.sh
```

- [ ] **Step 3: Run the new test and verify it fails before implementation**

Run: `make bin/test_format`

Expected: build fails with errors for missing unified API names such as `ChannelType`, `SignatureBytes`, `decoded_block_size`, or `compute_frame_hash`.

- [ ] **Step 4: Checkpoint**

Run: `git diff -- tests/test_format.cpp Makefile`

Expected: diff contains only the new test and Makefile test target changes.

## Task 2: Replace Format Codegen With Handwritten Unified Format

**Files:**
- Modify: `include/neotape/format.hpp`
- Modify: `src/neotape_format.cpp`
- Delete: `include/neotape/format_generated.hpp`
- Delete: `src/neotape_format_generated.cpp`
- Delete: `scripts/neotape_header_defs.py`
- Delete: `scripts/generate_neotape_parsers.py`

- [ ] **Step 1: Replace `include/neotape/format.hpp`**

Replace the file with declarations matching the approved design:

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace neotape {

inline constexpr std::size_t fixed_header_size = 512;
inline constexpr std::array<char, 8> magic = {'N', 'e', 'o', 'T', 'a', 'p', 'e', '\0'};
inline constexpr uint8_t header_version = 1;
inline constexpr uint32_t min_block_size = 4096;
inline constexpr uint32_t max_block_size = 8 * 1024 * 1024;

inline constexpr std::size_t nt_uuid_size = 37;
inline constexpr std::size_t archive_label_size = 65;
inline constexpr std::size_t signature_size = 72;

using HeaderBytes = std::array<uint8_t, fixed_header_size>;
using Hash = std::array<uint8_t, 32>;
using SignatureBytes = std::array<uint8_t, signature_size>;

enum class ChannelType : uint8_t {
    CH_CONTENT = 1,
    CH_METADATA = 2,
    ARCHIVE_END = 255,
};

inline constexpr uint64_t frame_flag_start = 1ull << 0;
inline constexpr uint64_t frame_flag_end = 1ull << 1;
inline constexpr uint64_t frame_flag_signed = 1ull << 2;
inline constexpr uint64_t frame_flag_clean_end = 1ull << 63;

inline constexpr bool has_frame_flag_start(uint64_t f) { return (f & frame_flag_start) != 0; }
inline constexpr bool has_frame_flag_end(uint64_t f) { return (f & frame_flag_end) != 0; }
inline constexpr bool has_frame_flag_signed(uint64_t f) { return (f & frame_flag_signed) != 0; }
inline constexpr bool has_frame_flag_clean_end(uint64_t f) { return (f & frame_flag_clean_end) != 0; }

struct FrameHeader {
    ChannelType channel_type{ChannelType::CH_CONTENT};
    uint16_t volume_block_size_kib{0};
    std::string archive_uuid{};
    std::string archive_label{};
    uint64_t volume_seq_num{0};
    uint64_t global_frame_seq_num{0};
    uint64_t logical_slice_seq_num{0};
    uint64_t frame_seq_num_within_channel{0};
    uint64_t frame_payload_size{0};
    uint64_t flags{0};
    SignatureBytes signature{};
    Hash frame_hash{};
};

HeaderBytes serialize_frame_header(const FrameHeader &header);
FrameHeader parse_frame_header(const uint8_t *data, std::size_t size);
FrameHeader parse_fixed_header(const uint8_t *data, std::size_t size);

std::string channel_type_name(ChannelType type);
std::string hash_hex(const Hash &hash);
Hash blake3_hash(const uint8_t *data, std::size_t size);
Hash compute_frame_hash(const uint8_t *data, std::size_t size);
uint32_t decoded_block_size(const FrameHeader &header);
bool valid_block_size(uint32_t block_size);

std::string utc_timestamp_now();
std::string make_uuid_v4();

} // namespace neotape
```

- [ ] **Step 2: Replace `src/neotape_format.cpp` with unified implementation**

Keep the existing `hash_hex`, `blake3_hash`, `utc_timestamp_now`, and `make_uuid_v4` behavior, and add these exact layout constants and validation rules in the same file:

```cpp
namespace {

using std::format;
using std::size_t;
using std::string;
using std::string_view;

constexpr size_t off_magic = 0;
constexpr size_t off_header_version = 8;
constexpr size_t off_channel_type = 9;
constexpr size_t off_volume_block_size_kib = 10;
constexpr size_t off_archive_uuid = 12;
constexpr size_t off_archive_label = 49;
constexpr size_t off_volume_seq_num = 114;
constexpr size_t off_global_frame_seq_num = 122;
constexpr size_t off_logical_slice_seq_num = 130;
constexpr size_t off_frame_seq_num_within_channel = 138;
constexpr size_t off_frame_payload_size = 146;
constexpr size_t off_flags = 154;
constexpr size_t off_reserved = 162;
constexpr size_t reserved_size = 246;
constexpr size_t off_signature = 408;
constexpr size_t off_frame_hash = 480;

constexpr uint64_t allowed_flags = neotape::frame_flag_start |
                                   neotape::frame_flag_end |
                                   neotape::frame_flag_signed |
                                   neotape::frame_flag_clean_end;
constexpr uint64_t reserved_flag_mask = ~allowed_flags;

void put_u16(HeaderBytes &bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xffu);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

void put_u64(HeaderBytes &bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i)
        bytes[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffu);
}

uint16_t get_u16(const uint8_t *bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1]) << 8;
}

uint64_t get_u64(const uint8_t *bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}

void put_fixed_string(HeaderBytes &bytes, size_t offset, size_t size,
                      string_view value) {
    size_t n = std::min(value.size(), size - 1);
    std::memcpy(bytes.data() + offset, value.data(), n);
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(offset + n),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + size), 0);
}

string get_nt_string(const uint8_t *bytes, size_t offset, size_t size) {
    if (bytes[offset + size - 1] != 0)
        throw std::runtime_error("fixed string field without trailing NUL");
    const auto *begin = reinterpret_cast<const char *>(bytes + offset);
    const char *end = std::find(begin, begin + size, '\0');
    return string(begin, end);
}

ChannelType parse_channel_type(uint8_t value) {
    switch (value) {
    case 1:
        return ChannelType::CH_CONTENT;
    case 2:
        return ChannelType::CH_METADATA;
    case 255:
        return ChannelType::ARCHIVE_END;
    default:
        throw std::runtime_error(format("unsupported channel type {}", value));
    }
}

void validate_header(const FrameHeader &h) {
    uint32_t block_size = decoded_block_size(h);
    if (h.frame_payload_size > block_size - fixed_header_size)
        throw std::runtime_error("frame payload exceeds record capacity");
    if ((h.flags & reserved_flag_mask) != 0)
        throw std::runtime_error("reserved frame flag bits are set");
    if (h.channel_type == ChannelType::ARCHIVE_END) {
        if (!has_frame_flag_start(h.flags) || !has_frame_flag_end(h.flags) ||
            !has_frame_flag_clean_end(h.flags))
            throw std::runtime_error("archive_end missing required flags");
        if (h.logical_slice_seq_num != 0)
            throw std::runtime_error("archive_end logical_slice_seq_num must be 0");
        if (h.frame_seq_num_within_channel != 1)
            throw std::runtime_error("archive_end frame_seq_num_within_channel must be 1");
    } else if (has_frame_flag_clean_end(h.flags)) {
        throw std::runtime_error("CLEAN_END is only valid for archive_end");
    }
}

} // namespace
```

Implement the declarations from `format.hpp` in `src/neotape_format.cpp` with these rules:

```cpp
HeaderBytes serialize_frame_header(const FrameHeader &header) {
    validate_header(header);
    HeaderBytes bytes{};
    for (size_t i = 0; i < magic.size(); ++i)
        bytes[off_magic + i] = static_cast<uint8_t>(magic[i]);
    bytes[off_header_version] = header_version;
    bytes[off_channel_type] = static_cast<uint8_t>(header.channel_type);
    put_u16(bytes, off_volume_block_size_kib, header.volume_block_size_kib);
    put_fixed_string(bytes, off_archive_uuid, nt_uuid_size, header.archive_uuid);
    put_fixed_string(bytes, off_archive_label, archive_label_size,
                     header.archive_label);
    put_u64(bytes, off_volume_seq_num, header.volume_seq_num);
    put_u64(bytes, off_global_frame_seq_num, header.global_frame_seq_num);
    put_u64(bytes, off_logical_slice_seq_num, header.logical_slice_seq_num);
    put_u64(bytes, off_frame_seq_num_within_channel,
            header.frame_seq_num_within_channel);
    put_u64(bytes, off_frame_payload_size, header.frame_payload_size);
    put_u64(bytes, off_flags, header.flags);
    std::copy(header.signature.begin(), header.signature.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(off_signature));
    std::copy(header.frame_hash.begin(), header.frame_hash.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(off_frame_hash));
    return bytes;
}

FrameHeader parse_fixed_header(const uint8_t *data, std::size_t size) {
    return parse_frame_header(data, size);
}

Hash compute_frame_hash(const uint8_t *data, std::size_t size) {
    FrameHeader header = parse_fixed_header(data, size);
    uint32_t expected_size = decoded_block_size(header);
    if (size != expected_size)
        throw std::runtime_error(format("frame size mismatch: expected {}, got {}",
                                        expected_size, size));
    std::vector<uint8_t> canonical(data, data + size);
    std::fill(canonical.begin() + static_cast<std::ptrdiff_t>(off_signature),
              canonical.begin() + static_cast<std::ptrdiff_t>(off_signature + signature_size),
              0);
    std::fill(canonical.begin() + static_cast<std::ptrdiff_t>(off_frame_hash),
              canonical.begin() + static_cast<std::ptrdiff_t>(off_frame_hash + 32),
              0);
    return blake3_hash(canonical.data(), canonical.size());
}

uint32_t decoded_block_size(const FrameHeader &header) {
    uint32_t block_size = static_cast<uint32_t>(header.volume_block_size_kib) * 1024u;
    if (!valid_block_size(block_size))
        throw std::runtime_error(format("invalid block size {}", block_size));
    return block_size;
}

bool valid_block_size(uint32_t block_size) {
    return block_size >= min_block_size && block_size <= max_block_size &&
           block_size % 1024u == 0;
}
```

Make `parse_frame_header()` populate all fields, verify magic/version, verify bytes `162..407` are zero, and call `validate_header()` before returning.

- [ ] **Step 3: Delete generated files and scripts**

Delete these files with `apply_patch` delete sections:

```text
include/neotape/format_generated.hpp
src/neotape_format_generated.cpp
scripts/neotape_header_defs.py
scripts/generate_neotape_parsers.py
```

- [ ] **Step 4: Run the format test**

Run: `make bin/test_format && bin/test_format`

Expected: build succeeds and output contains `test_format: ok`.

- [ ] **Step 5: Checkpoint**

Run: `git diff -- include/neotape/format.hpp src/neotape_format.cpp tests/test_format.cpp`

Expected: diff shows only unified format API and test changes for these files.

## Task 3: Remove Format Codegen From Build

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Remove generated variables and rules**

Remove these Makefile variables and rules:

```make
FORMAT_GEN_OBJ = src/neotape_format_generated.o
GENERATOR     = scripts/generate_neotape_parsers.py
GENERATED_HPP = include/neotape/format_generated.hpp
GENERATED_CPP = src/neotape_format_generated.cpp
$(GENERATED_HPP) $(GENERATED_CPP) &: $(GENERATOR) scripts/neotape_header_defs.py
	python3 $(GENERATOR)
$(FORMAT_GEN_OBJ): $(GENERATED_CPP) $(GENERATED_HPP) Makefile
	$(CXX) $(CXXFLAGS) -c $(GENERATED_CPP) -o $@
```

Change `CLANG_FORMAT_FILES` and `CLANG_TIDY_FILES` to stop filtering generated files:

```make
CLANG_FORMAT_FILES = $(wildcard src/*.cpp include/neotape/*.hpp include/neotape/*.h)
CLANG_TIDY_FILES = $(wildcard src/*.cpp)
```

Remove `$(FORMAT_GEN_OBJ)` from all link commands and target prerequisites.

- [ ] **Step 2: Remove generated-header dependencies**

Delete this dependency line:

```make
$(FORMAT_OBJ) $(TAPE_OBJ) $(TCP_SERVER_OBJ) $(WRITE_CMD_OBJ) $(READ_CMD_OBJ): $(GENERATED_HPP)
```

- [ ] **Step 3: Build the format test through Makefile**

Run: `make bin/test_format`

Expected: `bin/test_format` links without `src/neotape_format_generated.o`.

- [ ] **Step 4: Checkpoint**

Run: `git diff -- Makefile`

Expected: no codegen variables, no generated object, and `bin/test_format` is present.

## Task 4: Update TCP Protocol Message Types

**Files:**
- Modify: `include/neotape/tcp_protocol.hpp`
- Modify: `src/neotape_tcp_protocol.cpp`
- Modify: `tests/test_tcp_protocol.cpp`

- [ ] **Step 1: Update `MessageType`**

Replace the enum in `include/neotape/tcp_protocol.hpp` with:

```cpp
enum class MessageType : uint8_t {
    next_frame = 0x01,
    frame_record = 0x02,
    tape_eof = 0x03,
    error = 0x04,
    // ack_frame payload: uint64_t little-endian global frame sequence number.
    ack_frame = 0x05,
};
```

- [ ] **Step 2: Update message names**

In `src/neotape_tcp_protocol.cpp`, replace the `message_type_name()` switch cases with:

```cpp
case MessageType::next_frame:
    return "NEXT_FRAME";
case MessageType::frame_record:
    return "FRAME_RECORD";
case MessageType::tape_eof:
    return "TAPE_EOF";
case MessageType::error:
    return "ERROR";
case MessageType::ack_frame:
    return "ACK_FRAME";
```

- [ ] **Step 3: Update `tests/test_tcp_protocol.cpp`**

Change the empty-payload round-trip to use `MessageType::next_frame`:

```cpp
Message out{MessageType::next_frame, {}};
neotape::tcp::write_message(fds[1], out);

auto in = neotape::tcp::read_message(fds[0]);
if (!in.has_value())
    fail("expected a message for empty payload");
if (in->type != MessageType::next_frame)
    fail("wrong message type for empty payload");
if (!in->payload.empty())
    fail("expected empty payload");
```

- [ ] **Step 4: Run TCP protocol test**

Run: `make bin/test_tcp_protocol && bin/test_tcp_protocol`

Expected: output contains `test_tcp_protocol: ok`.

## Task 5: Update Archiver Record Generation And Serving

**Files:**
- Modify: `src/neotape_tcp_server.cpp`

- [ ] **Step 1: Remove volume-header construction**

Delete `make_volume_header()` and delete all `get_volume_header` / `volume_header` handling in `serve_client()` and the non-pax skeleton branch.

- [ ] **Step 2: Add unified record finalization helper**

Add this helper near `bytes_from_header_bytes()`:

```cpp
void copy_header_to_record(const HeaderBytes &header,
                           std::vector<std::byte> &record) {
    for (std::size_t i = 0; i < header.size(); ++i)
        record[i] = static_cast<std::byte>(header[i]);
}

void finalize_record_hash(FrameHeader &header, std::vector<std::byte> &record) {
    HeaderBytes header_bytes = serialize_frame_header(header);
    copy_header_to_record(header_bytes, record);
    header.frame_hash = compute_frame_hash(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
    header_bytes = serialize_frame_header(header);
    copy_header_to_record(header_bytes, record);
}
```

- [ ] **Step 3: Update `FrameBuilder::build_frame()`**

Use the unified header fields:

```cpp
FrameHeader fh;
fh.channel_type = ChannelType::CH_CONTENT;
fh.volume_block_size_kib = static_cast<uint16_t>(block_size / 1024u);
fh.archive_uuid = archive_uuid;
fh.archive_label = archive_name;
fh.volume_seq_num = volume_seq_num;
fh.global_frame_seq_num = seq_num;
fh.logical_slice_seq_num = slice;
fh.frame_seq_num_within_channel = 1;
fh.frame_payload_size = payload.size();
fh.flags = frame_flag_start | frame_flag_end;

HeaderBytes header = serialize_frame_header(fh);
std::vector<std::byte> record(block_size, std::byte{0});
copy_header_to_record(header, record);
std::copy(payload.begin(), payload.end(), record.begin() +
          static_cast<std::ptrdiff_t>(fixed_header_size));
finalize_record_hash(fh, record);
return record;
```

- [ ] **Step 4: Add archive-end record builder**

Add a helper near `FrameBuilder`:

```cpp
std::vector<std::byte> build_archive_end_record(uint32_t block_size,
                                                uint64_t volume_seq_num,
                                                const std::string &archive_uuid,
                                                const std::string &archive_name,
                                                uint64_t global_seq_num) {
    FrameHeader h;
    h.channel_type = ChannelType::ARCHIVE_END;
    h.volume_block_size_kib = static_cast<uint16_t>(block_size / 1024u);
    h.archive_uuid = archive_uuid;
    h.archive_label = archive_name;
    h.volume_seq_num = volume_seq_num;
    h.global_frame_seq_num = global_seq_num;
    h.logical_slice_seq_num = 0;
    h.frame_seq_num_within_channel = 1;
    h.frame_payload_size = 0;
    h.flags = frame_flag_start | frame_flag_end | frame_flag_clean_end;

    std::vector<std::byte> record(block_size, std::byte{0});
    finalize_record_hash(h, record);
    return record;
}
```

- [ ] **Step 5: Send archive end as `frame_record` and require ACK**

In `serve_client()`, replace `archive_end_sent` with:

```cpp
std::optional<uint64_t> archive_end_seq;
```

When `next->done` is popped:

```cpp
uint64_t ae_seq = next->global_seq_num + 1;
auto record = build_archive_end_record(opts.volume_block_size,
                                       state.next_volume_seq_num,
                                       archive_uuid,
                                       opts.archive_name,
                                       ae_seq);
retention.add(ae_seq, record);
seq = ae_seq;
archive_end_seq = ae_seq;
neotape::tcp::write_message(
    client, Message{MessageType::frame_record, std::move(record)});
break;
```

When sending a retained record, parse the retained header before writing it. If it is an archive-end frame, set `archive_end_seq = seq` so the following ACK can complete the archive:

```cpp
FrameHeader retained_header = parse_fixed_header(
    reinterpret_cast<const uint8_t *>(record.data()), record.size());
if (retained_header.channel_type == ChannelType::ARCHIVE_END)
    archive_end_seq = seq;
```

In `ack_frame` handling, after updating retained frames:

```cpp
if (archive_end_seq.has_value() && g >= *archive_end_seq)
    return ServeResult{true, volume_committed, frames_served};
```

At function exit, return archive complete only when the archive-end ACK arrived:

```cpp
return ServeResult{false, volume_committed, frames_served};
```

- [ ] **Step 6: Update non-pax skeleton mode**

Use `FrameBuilder` to produce valid unified frame records and use `build_archive_end_record()` for completion. Keep the existing `tape_eof` every fourth request behavior.

- [ ] **Step 7: Build archiver**

Run: `make bin/neotape-archiver`

Expected: build reaches either success or errors only in downstream files not yet updated in later tasks.

## Task 6: Update Writer Client Flow

**Files:**
- Modify: `src/neotape_write_cmd.cpp`

- [ ] **Step 1: Update the EOT comment**

Replace the comment above `write_trailing_filemark()` with:

```cpp
// Do not write a trailing filemark when EOT has been reached. On real tape
// hardware, issuing MTWEOF near the physical end of the medium can block while
// the kernel tries to flush data that cannot fit. The next volume resumes with
// the next uncommitted frame.
```

- [ ] **Step 2: Remove initial volume-header request and write**

Delete the block that sends `MessageType::get_volume_header`, reads `volume_header`, parses `HeaderType::volume`, and writes `vh->payload` before starting the writer thread.

Replace `const uint32_t volume_block_size = ...` with:

```cpp
std::optional<uint32_t> volume_block_size;
```

- [ ] **Step 3: Parse every `frame_record` as a unified frame**

In the `MessageType::frame_record` case, parse the header first:

```cpp
neotape::FrameHeader header = neotape::parse_fixed_header(
    reinterpret_cast<const uint8_t *>(msg->payload.data()),
    msg->payload.size());
uint32_t record_size = neotape::decoded_block_size(header);
if (!volume_block_size.has_value()) {
    volume_block_size = record_size;
    std::cerr << format("writer: first frame parsed block_size={}\n",
                        record_size);
}
if (msg->payload.size() != *volume_block_size)
    joined_fail(format("frame size mismatch: expected {}, got {}",
                       *volume_block_size, msg->payload.size()));
```

- [ ] **Step 4: Handle archive end inside `frame_record`**

Before queuing normal frames, add:

```cpp
if (header.channel_type == neotape::ChannelType::ARCHIVE_END) {
    NEOTAPE_DEBUG("writer: archive_end frame, draining queue\n");
    {
        std::lock_guard lock(wstate.output_mtx);
        wstate.final_drain.store(true);
    }
    wstate.output_cv.notify_all();
    for (;;) {
        std::unique_lock lock(wstate.output_mtx);
        if (wstate.output_queue.empty())
            break;
        lock.unlock();
        if (wstate.writer_error.load())
            joined_fail(wstate.writer_error_text);
        if (wstate.eot_reached.load()) {
            joiner.join();
            write_trailing_filemark(output.device.get());
            uint64_t final_seq = wstate.last_written_seq.load();
            if (final_seq > 0)
                write_msg(Message{MessageType::ack_frame,
                                  uint64_to_le_bytes(final_seq)});
            std::cerr << format("writer: reached end of tape after {} frames\n",
                                final_seq);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    joiner.join();
    write_bytes(msg->payload);
    write_msg(Message{MessageType::ack_frame,
                      uint64_to_le_bytes(header.global_frame_seq_num)});
    std::cerr << format("writer: received archive end at frame {}\n",
                        header.global_frame_seq_num);
    return 0;
}
```

Then queue content/metadata frames with:

```cpp
uint64_t gseq = header.global_frame_seq_num;
std::unique_lock lock(wstate.output_mtx);
wstate.output_queue.push_back(PendingFrame{gseq, std::move(msg->payload)});
wstate.output_cv.notify_one();
```

- [ ] **Step 5: Remove `archive_end_header` switch case**

Delete the old `case MessageType::archive_end_header:` block.

- [ ] **Step 6: Build writer**

Run: `make bin/neotape-write`

Expected: writer builds or errors only in files scheduled for later tasks.

## Task 7: Update Spool, Read, And Probe Consumers

**Files:**
- Modify: `src/neotape_tape.cpp`
- Modify: `src/neotape_read_cmd.cpp`
- Modify: `tests/lto-variable-block-record-probe.cpp`

- [ ] **Step 1: Update spool naming in `src/neotape_tape.cpp`**

Change `spool_suffix_for_header()` to accept `const neotape::FrameHeader &header` and use:

```cpp
switch (header.channel_type) {
case neotape::ChannelType::CH_CONTENT:
case neotape::ChannelType::CH_METADATA:
    return format("slice-{:06}", header.logical_slice_seq_num);
case neotape::ChannelType::ARCHIVE_END:
    return "archive-end";
}
throw std::runtime_error("unsupported spool channel type");
```

Change `parse_spool_header_file()` to return `neotape::FrameHeader`.

Change `finalize_current_file()` to:

```cpp
auto header = parse_spool_header_file(current_path_);
current_block_size_ = neotape::decoded_block_size(header);
fs::path final_path = spool_final_path(root_, current_file_num_, header);
fs::rename(current_path_, final_path);
```

- [ ] **Step 2: Update `SpoolSourceReader` in `src/neotape_read_cmd.cpp`**

Replace the old `ParsedHeader` logic with:

```cpp
neotape::FrameHeader header = neotape::parse_fixed_header(
    reinterpret_cast<const uint8_t *>(pending_.data()),
    neotape::fixed_header_size);

size_t record_size = neotape::decoded_block_size(header);
```

- [ ] **Step 3: Update read success checks in `src/neotape_read_cmd.cpp`**

Replace `has_volume_header` with `has_content_or_metadata_frame`. In the parse block use:

```cpp
neotape::FrameHeader header = neotape::parse_fixed_header(
    reinterpret_cast<const uint8_t *>(record->data()),
    record->size());
if (header.channel_type == neotape::ChannelType::CH_CONTENT ||
    header.channel_type == neotape::ChannelType::CH_METADATA)
    has_content_or_metadata_frame = true;
if (header.channel_type == neotape::ChannelType::ARCHIVE_END)
    has_archive_end_frame = true;
```

Update the summary output to print `content/metadata frame: yes|no`, and fail if no content/metadata frame or no archive end frame.

- [ ] **Step 4: Update `tests/lto-variable-block-record-probe.cpp`**

Remove `#include "neotape/tape_navigator.hpp"`, remove `navigator_read()`, and change the write mode from `write-volume` to `write-frame`. Build a `FrameHeader` with `ChannelType::CH_CONTENT`, `volume_block_size_kib = record_size / 1024`, one-byte payload, `START | END`, compute the frame hash with `compute_frame_hash()`, and write the full record.

In read output, print:

```cpp
auto parsed = neotape::parse_fixed_header(buf.data(), static_cast<std::size_t>(n));
std::cout << "channel_type=" << neotape::channel_type_name(parsed.channel_type) << '\n';
std::cout << "block_size=" << neotape::decoded_block_size(parsed) << '\n';
```

Update usage string to:

```cpp
"usage: lto-variable-block-record-probe <write-frame|read-16m|read-2m|read-4m|read-8m|read-512k> /dev/nst0"
```

- [ ] **Step 5: Build read-related targets**

Run: `make bin/neotape-read bin/neotape-write`

Expected: both targets build or only archiver issues remain from Task 5.

## Task 8: Update Smoke Tests For Unified Header Layout

**Files:**
- Modify: `tests/smoke_tcp_archive.sh`
- Modify: `tests/smoke_tcp_archive_multi.sh`

- [ ] **Step 1: Update `tests/smoke_tcp_archive.sh` constants and checks**

Set:

```sh
HEADER_SIZE=512
```

Change the size check to require one frame and one archive-end record:

```sh
if [ "$ACTUAL" -lt $((BLOCK * 2)) ]; then
    echo "smoke_tcp_archive: output too small: $ACTUAL"
    exit 1
fi
```

Change byte-9 checks:

```sh
CTYPE=$(od -An -tx1 -N1 -j9 "$OUT" | tr -d ' \n')
if [ "$CTYPE" != "01" ]; then
    echo "smoke_tcp_archive: expected ch_content channel 0x01, got $CTYPE"
    exit 1
fi

AE_OFFSET=$((ACTUAL - BLOCK))
AE_MAGIC=$(od -An -tx1 -N8 -j$AE_OFFSET "$OUT" | tr -d ' \n')
AE_TYPE=$(od -An -tx1 -N1 -j$((AE_OFFSET + 9)) "$OUT" | tr -d ' \n')
if [ "$AE_MAGIC" != "4e656f5461706500" ] || [ "$AE_TYPE" != "ff" ]; then
    echo "smoke_tcp_archive: archive end frame mismatch"
    exit 1
fi
```

- [ ] **Step 2: Update `tests/smoke_tcp_archive_multi.sh` constants and checks**

Set:

```sh
HEADER_SIZE=512
```

Replace the "Verify two distinct volume headers" block with first-record channel checks:

```sh
for spool in "$SPOOL1" "$SPOOL2"; do
    FIRST=$(find "$spool" -maxdepth 1 -type f -name '*.nts' | sort | head -n1)
    if [ -z "$FIRST" ]; then
        echo "smoke_tcp_archive_multi: missing spool files in $spool"
        exit 1
    fi
    MAGIC=$(od -An -tx1 -N8 -j0 "$FIRST" | tr -d ' \n')
    if [ "$MAGIC" != "4e656f5461706500" ]; then
        echo "smoke_tcp_archive_multi: bad magic in $spool"
        exit 1
    fi
    CTYPE=$(od -An -tx1 -N1 -j9 "$FIRST" | tr -d ' \n')
    if [ "$CTYPE" != "01" ] && [ "$CTYPE" != "ff" ]; then
        echo "smoke_tcp_archive_multi: expected frame channel in $spool, got $CTYPE"
        exit 1
    fi
done
```

Change archive-end offset to the start of the last full block:

```sh
AE_OFFSET=$((LAST_SIZE - BLOCK))
AE_MAGIC=$(od -An -tx1 -N8 -j$AE_OFFSET "$LAST" | tr -d ' \n')
AE_TYPE=$(od -An -tx1 -N1 -j$((AE_OFFSET + 9)) "$LAST" | tr -d ' \n')
if [ "$AE_MAGIC" != "4e656f5461706500" ] || [ "$AE_TYPE" != "ff" ]; then
    echo "smoke_tcp_archive_multi: second spool missing archive end frame"
    exit 1
fi
```

- [ ] **Step 3: Run shell syntax checks**

Run: `sh -n tests/smoke_tcp_archive.sh && sh -n tests/smoke_tcp_archive_multi.sh`

Expected: no output and exit code 0.

## Task 9: Update Documentation References

**Files:**
- Modify: `AGENTS.md`
- Delete: `docs/implementation/header-codegen.md`
- Delete: `docs/implementation/header-codegen-plan.md`
- Modify: `docs/implementation/phase-1-header-layout.md`

- [ ] **Step 1: Update `AGENTS.md`**

Replace the `## Python Codegen` section with:

```md
## Header Parser

- NeoTape uses one handwritten unified 512-byte Frame Header parser/serializer.
- `include/neotape/format.hpp` declares the format API.
- `src/neotape_format.cpp` owns byte offsets, parsing, serialization, and frame-hash helpers.
- Do not reintroduce header codegen unless the format grows multiple independent layouts again.
```

- [ ] **Step 2: Delete obsolete implementation docs**

Delete:

```text
docs/implementation/header-codegen.md
docs/implementation/header-codegen-plan.md
```

- [ ] **Step 3: Update `docs/implementation/phase-1-header-layout.md`**

Remove the block that says Python codegen is the single source of truth. Replace it with:

```md
> The active implementation now uses a handwritten unified Frame Header parser in
> `src/neotape_format.cpp`. The normative binary layout is `docs/spec/01-frame-header.md`.
```

- [ ] **Step 4: Check stale references outside historical plans**

Run: `rg "neotape_header_defs|generate_neotape_parsers|format_generated|Python codegen" AGENTS.md docs README.md Makefile include src scripts tests`

Expected: no matches outside deleted-file paths or historical `docs/superpowers/plans/` files.

## Task 10: Full Build, Compile Database, And Test Run

**Files:**
- Modify: `compile_commands.json`

- [ ] **Step 1: Build all targets**

Run: `make -j "$(nproc)"`

Expected: all binaries in `EXE` build successfully.

- [ ] **Step 2: Run full test suite**

Run: `make test`

Expected output contains:

```text
test_pax_pipeline: ok
test_tcp_protocol: ok
test_format: ok
smoke_tcp_archive: ok
smoke_tcp_archive_multi: ok
smoke_mt_pax_parity: ok
```

- [ ] **Step 3: Regenerate compile commands**

Run: `make compile_commands`

Expected: output reports regenerated `compile_commands.json`; the file no longer contains `src/neotape_format_generated.cpp`.

- [ ] **Step 4: Verify no stale generated references remain in active code**

Run: `rg "format_generated|serialize_volume_header|serialize_archive_end_header|HeaderType|VolumeHeader|ArchiveEndHeader|PayloadProfile|FrameContentType|get_volume_header|volume_header|archive_end_header" include src tests Makefile AGENTS.md docs/README.md docs/implementation`

Expected: no matches.

- [ ] **Step 5: Final worktree review**

Run: `git status --short` and `git diff --stat`

Expected: only intended files from this plan are modified/deleted/created. Do not revert unrelated `docs/spec/` changes made by the user.
