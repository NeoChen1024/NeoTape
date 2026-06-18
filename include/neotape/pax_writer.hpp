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
    std::function<void(uint64_t)> begin_slice = [](uint64_t) {};
    std::function<void(PaxChunk)> write_chunk;
    std::function<void(uint64_t)> end_slice = [](uint64_t) {};
    std::function<bool()> progress_paused = [] { return false; };
};

struct PaxWriteResult {
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
    uint64_t walked_entries = 0;
    uint64_t slices = 0;
    std::string blake3_hex;
};

struct PaxLocalOutputOptions {
    std::string output_path = "-";
    std::optional<std::string> slice_output_prefix;
};

struct PaxLocalOutputResult {
    PaxWriteResult write_result;
    std::string output_target;
};

PaxWriteResult write_pax(const PaxWriterOptions &opts,
                         PaxWriterCallbacks callbacks);
PaxLocalOutputResult write_pax_to_local_output(
    const PaxWriterOptions &writer_opts, const PaxLocalOutputOptions &out_opts);

} // namespace neotape
