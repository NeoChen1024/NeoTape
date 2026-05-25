#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace neotape {

struct PaxChunk {
    uint64_t slice = 0;
    std::span<const std::byte> bytes;
};

struct PaxWriterOptions {
    std::string output_name = "-";
    std::vector<std::string> sources;
    std::optional<std::filesystem::path> plan_path;
    int verbose = 0;
    bool one_file_system = false;
    std::optional<std::string> chdir_dir;
    std::size_t output_buf_size = 64UL * 1024 * 1024;
    unsigned buffer_percent = 0;
    unsigned io_thread = 1;
};

struct PaxWriterCallbacks {
    std::function<void(uint64_t)> begin_slice;
    std::function<void(PaxChunk)> write_chunk;
    std::function<void(uint64_t)> end_slice;
    std::function<bool()> progress_paused;
};

struct PaxWriteResult {
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
    uint64_t walked_entries = 0;
    uint64_t slices = 0;
    std::string blake3_hex;
};

PaxWriteResult write_pax(const PaxWriterOptions &opts,
                         PaxWriterCallbacks callbacks);

void ensure_utf8_ctype_locale();

} // namespace neotape
