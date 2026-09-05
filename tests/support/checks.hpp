#pragma once

#include "process.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace neotape::test {
inline std::string output(const ProcessResult &result) {
    return result.standard_output + result.standard_error;
}

inline void require_contains(const ProcessResult &result,
                             std::string_view text) {
    INFO("stdout:\n" << result.standard_output);
    INFO("stderr:\n" << result.standard_error);
    REQUIRE(output(result).find(text) != std::string::npos);
}

inline std::filesystem::path content_file(const std::filesystem::path &spool) {
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(spool)) {
        if (entry.is_regular_file() && entry.path().extension() == ".nts" &&
            entry.path().filename().string().find(".slice-") !=
                std::string::npos)
            files.push_back(entry.path());
    }
    // These fixtures contain one content tape file; do not silently choose one.
    REQUIRE(files.size() == 1);
    return files.front();
}

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

inline std::vector<std::byte> read_bytes(const std::filesystem::path &path) {
    auto text = read_file(path);
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (unsigned char byte : text)
        bytes.push_back(static_cast<std::byte>(byte));
    return bytes;
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
