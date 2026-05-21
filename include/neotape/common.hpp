#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace neotape {

uint64_t parse_size(std::string_view text, std::string_view name);
std::string humanize_number(std::size_t number);

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
