#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace neotape {

extern bool g_debug;

// Serialize diagnostics with the live progress display.  Ordinary messages
// terminate an active progress line before they are written.
void write_diagnostic(std::string_view message);
void write_progress(std::string_view message);
void finish_progress();

std::string escape_bytes_for_diagnostic(std::string_view bytes);

#define NEOTAPE_DEBUG(...)                                                     \
    do {                                                                       \
        if (neotape::g_debug)                                                  \
            neotape::write_diagnostic(std::format(__VA_ARGS__));               \
    } while (0)

uint64_t parse_uint(std::string_view text, std::string_view name,
                    uint64_t minimum = 0,
                    uint64_t maximum = std::numeric_limits<uint64_t>::max());
uint64_t parse_size(std::string_view text, std::string_view name,
                    uint64_t maximum = std::numeric_limits<uint64_t>::max());
std::string hex_encode(std::span<const uint8_t> bytes);
std::string humanize_number(std::size_t number);
void ensure_utf8_ctype_locale();

struct SourceSpec {
    std::string archive_prefix;
    std::filesystem::path open_path;
};

bool has_trailing_slash(std::string_view path);
std::string strip_trailing_slashes(std::string_view path);
SourceSpec make_source_spec(const std::string &arg);
std::string archive_path_for_source(const SourceSpec &spec,
                                    const std::string &source_path);

} // namespace neotape
