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

TEST_CASE("display escaping keeps UTF-8 text and escapes unsafe bytes",
          "[unit][common]") {
    using neotape::escape_bytes_for_diagnostic;
    // U+4E2D U+6587 (3-byte), U+00E9 (2-byte), U+1F4BE (4-byte).
    REQUIRE(escape_bytes_for_diagnostic(
                "dir/\xe4\xb8\xad\xe6\x96\x87-\xc3\xa9-\xf0\x9f\x92\xbe.txt") ==
            "dir/\xe4\xb8\xad\xe6\x96\x87-\xc3\xa9-\xf0\x9f\x92\xbe.txt");
    REQUIRE(escape_bytes_for_diagnostic("a\\b\n\t\x7f") ==
            "a\\\\b\\x0a\\x09\\x7f");
    // Lone continuation, truncated sequence, and Latin-1 byte.
    REQUIRE(escape_bytes_for_diagnostic("\x82") == "\\x82");
    REQUIRE(escape_bytes_for_diagnostic("\xe4\xb8") == "\\xe4\\xb8");
    REQUIRE(escape_bytes_for_diagnostic("caf\xe9") == "caf\\xe9");
    // Overlong '/', UTF-16 surrogate, beyond U+10FFFF, C1 control U+0085.
    REQUIRE(escape_bytes_for_diagnostic("\xc0\xaf") == "\\xc0\\xaf");
    REQUIRE(escape_bytes_for_diagnostic("\xed\xa0\x80") == "\\xed\\xa0\\x80");
    REQUIRE(escape_bytes_for_diagnostic("\xf4\x90\x80\x80") ==
            "\\xf4\\x90\\x80\\x80");
    REQUIRE(escape_bytes_for_diagnostic("\xc2\x85") == "\\xc2\\x85");
    // U+202E right-to-left override: "evil<RLO>txt.exe" must not render as
    // "evilexe.txt". Likewise U+2066 isolate, U+200B zero-width space,
    // U+FEFF, and tag character U+E0041.
    REQUIRE(escape_bytes_for_diagnostic("evil\xe2\x80\xaetxt.exe") ==
            "evil\\xe2\\x80\\xaetxt.exe");
    REQUIRE(escape_bytes_for_diagnostic("\xe2\x81\xa6") == "\\xe2\\x81\\xa6");
    REQUIRE(escape_bytes_for_diagnostic("a\xe2\x80\x8b"
                                        "b") == "a\\xe2\\x80\\x8b"
                                                "b");
    REQUIRE(escape_bytes_for_diagnostic("\xef\xbb\xbf") == "\\xef\\xbb\\xbf");
    REQUIRE(escape_bytes_for_diagnostic("\xf3\xa0\x81\x81") ==
            "\\xf3\\xa0\\x81\\x81");
    // Neighbouring ordinary punctuation U+2010 and U+2030 stay readable.
    REQUIRE(escape_bytes_for_diagnostic("\xe2\x80\x90\xe2\x80\xb0") ==
            "\xe2\x80\x90\xe2\x80\xb0");
}
