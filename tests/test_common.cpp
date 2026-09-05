#include "neotape/common.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>

TEST_CASE("unsigned numeric arguments reject malformed and out-of-range input",
          "[unit][cli]") {
    using neotape::parse_uint;
    REQUIRE(parse_uint("0", "count") == 0);
    REQUIRE(parse_uint("100", "percent", 0, 100) == 100);
    REQUIRE(parse_uint("18446744073709551615", "count") ==
            std::numeric_limits<uint64_t>::max());
    for (const char *text :
         {"", "-1", "+1", " 1", "1 ", "1x", "18446744073709551616"}) {
        CAPTURE(text);
        REQUIRE_THROWS_AS(parse_uint(text, "count"), std::invalid_argument);
    }
    REQUIRE_THROWS_AS(parse_uint("101", "percent", 0, 100), std::out_of_range);
    REQUIRE_THROWS_AS(parse_uint("0", "count", 1, 10), std::out_of_range);
    REQUIRE_THROWS_AS(parse_uint("4294967296", "threads", 0, UINT32_MAX),
                      std::out_of_range);
}

TEST_CASE("byte sizes check scaled destination limits", "[unit][cli]") {
    using neotape::parse_size;
    REQUIRE(parse_size("4k", "size") == 4096);
    REQUIRE(parse_size("2T", "size") == 2ULL * 1024 * 1024 * 1024 * 1024);
    REQUIRE(parse_size("4K", "size", 4096) == 4096);
    for (const char *text :
         {"0", "K", "-1", "-1K", "1KB", "18446744073709551615K"}) {
        CAPTURE(text);
        REQUIRE_THROWS(parse_size(text, "size"));
    }
    REQUIRE_THROWS(parse_size("4G", "block size", UINT32_MAX));
    REQUIRE_THROWS(parse_size("1K", "size", 1023));
}

TEST_CASE("hex encoding preserves zero and high bytes", "[unit][common]") {
    std::array<uint8_t, 5> bytes{0, 1, 15, 128, 255};
    REQUIRE(neotape::hex_encode(bytes) == "00010f80ff");
    REQUIRE(neotape::hex_encode({}).empty());
}
