#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace neotape {

extern bool g_debug;

// Executable name that prefixes fatal diagnostics; each main() sets it first.
extern std::string_view program_name;
// Report and exit: status 1 for runtime failures, 2 for usage errors.
[[noreturn]] void fail(std::string_view message);
[[noreturn]] void usage_error(std::string_view message);

// Serialize diagnostics with the live progress display.  Ordinary messages
// terminate an active progress line before they are written.
void write_diagnostic(std::string_view message);
void write_progress(std::string_view message);
void finish_progress();

// Make path bytes safe to print on one line: printable ASCII and well-formed
// UTF-8 pass through; backslash, control characters, bytes that are not
// valid UTF-8, and code points that reorder or hide text (bidirectional
// controls, zero-width characters) become `\\` or `\xNN`.
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

SourceSpec make_source_spec(const std::string &arg);
std::string archive_path_for_source(const SourceSpec &spec,
                                    const std::string &source_path);

// Owns a stdio stream for one scope. A standard stream is flushed but never
// closed. close() reports buffered write errors; the destructor does not.
class StdioFile {
  public:
    // Adopt a standard stream such as stdout.
    explicit StdioFile(FILE *standard_stream)
        : file_(standard_stream), owned_(false) {}
    ~StdioFile();
    StdioFile(StdioFile &&other) noexcept
        : file_(std::exchange(other.file_, nullptr)), owned_(other.owned_) {}
    StdioFile(const StdioFile &) = delete;
    StdioFile &operator=(const StdioFile &) = delete;
    StdioFile &operator=(StdioFile &&) = delete;

    // fopen(); throws std::runtime_error naming `path` on failure.
    static StdioFile open(const std::string &path, const char *mode);

    [[nodiscard]] FILE *get() const { return file_; }
    // Flush a standard stream or close an owned one; throws on failure.
    void close(std::string_view name);

  private:
    StdioFile(FILE *file, bool owned) : file_(file), owned_(owned) {}
    FILE *file_;
    bool owned_;
};

} // namespace neotape
