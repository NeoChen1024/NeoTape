#pragma once

#include "process.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace neotape::test {
inline void require_success(const ProcessResult &result) {
    INFO("stdout:\n" << result.standard_output);
    INFO("stderr:\n" << result.standard_error);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
}

inline std::string read_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), {}};
}

inline void write_pattern(const std::filesystem::path &path, size_t size) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    std::string block(64 * 1024, '\0');
    for (size_t i = 0; i < block.size(); ++i)
        block[i] = static_cast<char>((i * 29 + 7) % 251);
    while (size) {
        size_t count = std::min(size, block.size());
        output.write(block.data(), count);
        size -= count;
    }
    REQUIRE(output.good());
}
} // namespace neotape::test
