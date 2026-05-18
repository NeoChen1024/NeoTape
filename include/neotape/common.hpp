#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace neotape {

uint64_t parse_size(std::string_view text, std::string_view name);
std::string humanize_number(std::size_t number);

} // namespace neotape
