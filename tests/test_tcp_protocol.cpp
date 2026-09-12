#include "neotape/tcp_protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fcntl.h>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using neotape::tcp::Message;
using neotape::tcp::MessageType;

struct Pipe {
    int fds[2];
    Pipe() { REQUIRE(::pipe(fds) == 0); }
    ~Pipe() {
        ::close(fds[0]);
        if (fds[1] >= 0)
            ::close(fds[1]);
    }
    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;
    void finish() {
        REQUIRE(::close(fds[1]) == 0);
        fds[1] = -1;
    }
    void send(std::span<const uint8_t> bytes) {
        // Fixtures fit in PIPE_BUF and use a blocking pipe with no other
        // writer.
        REQUIRE(::write(fds[1], bytes.data(), bytes.size()) ==
                static_cast<ssize_t>(bytes.size()));
    }
};

// Literal protocol assignments, independent of MessageType's numeric values.
constexpr std::array wire_types{
    std::pair{MessageType::next_frame, uint8_t{0x01}},
    std::pair{MessageType::frame_record, uint8_t{0x02}},
    std::pair{MessageType::tape_eof, uint8_t{0x03}},
    std::pair{MessageType::error, uint8_t{0x04}},
    std::pair{MessageType::ack_frame, uint8_t{0x05}},
    std::pair{MessageType::auth_challenge, uint8_t{0x06}},
    std::pair{MessageType::auth_response, uint8_t{0x07}}};
} // namespace

TEST_CASE("protocol message type bytes are stable", "[unit][protocol]") {
    auto const [type, byte] = GENERATE(from_range(wire_types));
    CAPTURE(byte);
    Pipe pipe;
    neotape::tcp::write_message(pipe.fds[1], Message{type});
    pipe.finish();
    std::array<uint8_t, 10> actual{};
    REQUIRE(::read(pipe.fds[0], actual.data(), actual.size()) == 9);
    std::array<uint8_t, 10> expected{byte, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE(actual == expected);
}

TEST_CASE("protocol reader accepts fixed message type bytes",
          "[unit][protocol]") {
    auto const [type, byte] = GENERATE(from_range(wire_types));
    Pipe pipe;
    pipe.send(std::array<uint8_t, 9>{byte, 0, 0, 0, 0, 0, 0, 0, 0});
    pipe.finish();
    auto const message = neotape::tcp::read_message(pipe.fds[0]);
    REQUIRE(message.has_value());
    REQUIRE(message->type == type);
    REQUIRE(message->payload.empty());
    REQUIRE_FALSE(neotape::tcp::read_message(pipe.fds[0]).has_value());
}

TEST_CASE("protocol length and payload match a fixed wire vector",
          "[unit][protocol]") {
    std::vector<std::byte> payload(256);
    std::vector<uint8_t> wire{0x02, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    for (unsigned i = 0; i < 256; ++i) {
        payload[i] = static_cast<std::byte>(i);
        wire.push_back(static_cast<uint8_t>(i));
    }
    SECTION("writer produces little-endian length and exact payload") {
        Pipe pipe;
        neotape::tcp::write_message(pipe.fds[1],
                                    {MessageType::frame_record, payload});
        pipe.finish();
        std::vector<uint8_t> actual(wire.size() + 1);
        REQUIRE(::read(pipe.fds[0], actual.data(), actual.size()) ==
                static_cast<ssize_t>(wire.size()));
        actual.resize(wire.size());
        REQUIRE(actual == wire);
    }
    SECTION("reader consumes independent bytes") {
        Pipe pipe;
        pipe.send(wire);
        pipe.finish();
        auto const message = neotape::tcp::read_message(pipe.fds[0]);
        REQUIRE(message.has_value());
        REQUIRE(message->type == MessageType::frame_record);
        REQUIRE(message->payload == payload);
    }
}

TEST_CASE("protocol preserves consecutive ACK and authentication payloads",
          "[unit][protocol]") {
    Pipe pipe;
    std::array messages{
        Message{MessageType::ack_frame,
                {std::byte{0xf0}, std::byte{0xde}, std::byte{0xbc},
                 std::byte{0x9a}, std::byte{0x78}, std::byte{0x56},
                 std::byte{0x34}, std::byte{0x12}}},
        Message{MessageType::auth_challenge,
                std::vector<std::byte>(32, std::byte{0x5a})},
        Message{MessageType::auth_response,
                std::vector<std::byte>(64, std::byte{0xa5})}};
    for (auto const &message : messages)
        neotape::tcp::write_message(pipe.fds[1], message);
    pipe.finish();
    for (auto const &expected : messages) {
        auto const actual = neotape::tcp::read_message(pipe.fds[0]);
        REQUIRE(actual.has_value());
        REQUIRE(actual->type == expected.type);
        REQUIRE(actual->payload == expected.payload);
    }
    REQUIRE_FALSE(neotape::tcp::read_message(pipe.fds[0]).has_value());
}

TEST_CASE("protocol distinguishes clean EOF from a truncated header",
          "[unit][protocol]") {
    auto const size = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U);
    std::array<uint8_t, 9> header{0x02, 1, 0, 0, 0, 0, 0, 0, 0};
    Pipe pipe;
    pipe.send(std::span(header).first(size));
    pipe.finish();
    if (size == 0)
        REQUIRE_FALSE(neotape::tcp::read_message(pipe.fds[0]).has_value());
    else
        REQUIRE_THROWS_AS(neotape::tcp::read_message(pipe.fds[0]),
                          std::runtime_error);
}

TEST_CASE("protocol rejects truncated payload", "[unit][protocol]") {
    Pipe pipe;
    pipe.send(std::array<uint8_t, 10>{0x02, 2, 0, 0, 0, 0, 0, 0, 0, 0xab});
    pipe.finish();
    REQUIRE_THROWS_AS(neotape::tcp::read_message(pipe.fds[0]),
                      std::runtime_error);
}

TEST_CASE("protocol rejects unknown type before reading payload",
          "[unit][protocol]") {
    auto const type = GENERATE(0x00, 0x08, 0xff);
    Pipe pipe;
    pipe.send(std::array<uint8_t, 9>{static_cast<uint8_t>(type), 0, 0, 0, 0, 0,
                                     0, 0, 0});
    pipe.finish();
    REQUIRE_THROWS_AS(neotape::tcp::read_message(pipe.fds[0]),
                      std::runtime_error);
}

TEST_CASE("protocol rejects oversized length before reading payload",
          "[unit][protocol]") {
    // 16 MiB + 1 and UINT64_MAX, encoded independently of the codec/limit
    // constant.
    auto const length = GENERATE(
        (std::array<uint8_t, 8>{1, 0, 0, 1, 0, 0, 0, 0}),
        (std::array<uint8_t, 8>{255, 255, 255, 255, 255, 255, 255, 255}));
    Pipe pipe;
    pipe.send(std::array<uint8_t, 1>{0x02});
    pipe.send(length);
    // Leave the writer open: accepting the length would block, not fail at EOF.
    // A nonblocking read makes such a regression fail immediately with EAGAIN.
    int const flags = ::fcntl(pipe.fds[0], F_GETFL);
    REQUIRE(flags >= 0);
    REQUIRE(::fcntl(pipe.fds[0], F_SETFL, flags | O_NONBLOCK) == 0);
    REQUIRE_THROWS_WITH(neotape::tcp::read_message(pipe.fds[0]),
                        Catch::Matchers::ContainsSubstring("exceeds maximum"));
}

TEST_CASE("protocol parses TCP addresses", "[unit][protocol]") {
    auto const [text, host] =
        GENERATE(std::pair{"tcp://127.0.0.1:9123", "127.0.0.1"},
                 std::pair{"tcp://[::1]:9123", "::1"},
                 std::pair{"tcp://::1:9123", "::1"},
                 std::pair{"tcp://0.0.0.0:9123", "0.0.0.0"});
    auto const address = neotape::tcp::parse_address(text);
    REQUIRE_FALSE(address.is_unix);
    REQUIRE(address.host == host);
    REQUIRE(address.port == "9123");
}

TEST_CASE("protocol parses Unix addresses", "[unit][protocol]") {
    auto const address =
        neotape::tcp::parse_address("unix:///tmp/neotape.sock");
    REQUIRE(address.is_unix);
    REQUIRE(address.path == "/tmp/neotape.sock");
}

TEST_CASE("protocol rejects unsupported addresses", "[unit][protocol]") {
    auto const text = GENERATE("tcp://localhost", "http://example.com:80", "");
    REQUIRE_THROWS_AS(neotape::tcp::parse_address(text), std::runtime_error);
}
