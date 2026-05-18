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
    std::string original;
    std::filesystem::path open_path;
    std::filesystem::path open_parent;
    std::filesystem::path archive_root;
};

bool has_trailing_slash(std::string_view path);
std::string strip_trailing_slashes(std::string_view path);
SourceSpec make_source_spec(const std::string &arg);
std::filesystem::path drop_first_component(const std::filesystem::path &path);
std::string archive_path_for_source(const SourceSpec &spec, const std::string &source_path);

} // namespace neotape
