#include "neotape/tcp_protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

using neotape::tcp::Message;
using neotape::tcp::MessageType;

[[noreturn]] static void fail(const std::string &msg) {
    std::cerr << "test_tcp_protocol: " << msg << "\n";
    std::exit(1);
}

int main() {
    int fds[2];
    if (pipe(fds) != 0) {
        fail("pipe failed");
    }

    // Empty payload round-trip.
    {
        Message out{MessageType::next_frame, {}};
        neotape::tcp::write_message(fds[1], out);

        auto in = neotape::tcp::read_message(fds[0]);
        if (!in.has_value()) {
            fail("expected a message for empty payload");
        }
        if (in->type != MessageType::next_frame) {
            fail("wrong message type for empty payload");
        }
        if (!in->payload.empty()) {
            fail("expected empty payload");
        }
    }

    // 256-byte payload round-trip.
    {
        std::vector<std::byte> payload;
        payload.reserve(256);
        for (int i = 0; i < 256; ++i) {
            payload.push_back(static_cast<std::byte>(i));
        }

        Message out{MessageType::frame_record, std::move(payload)};
        neotape::tcp::write_message(fds[1], out);

        auto in = neotape::tcp::read_message(fds[0]);
        if (!in.has_value()) {
            fail("expected a message");
        }
        if (in->type != MessageType::frame_record) {
            fail("wrong message type");
        }
        if (in->payload.size() != 256) {
            fail("wrong payload size");
        }
        for (int i = 0; i < 256; ++i) {
            if (static_cast<uint8_t>(in->payload[i]) !=
                static_cast<uint8_t>(i)) {
                fail("payload mismatch");
            }
        }
    }

    // Multiple sequential messages.
    {
        for (int i = 0; i < 4; ++i) {
            std::vector<std::byte> payload = {
                static_cast<std::byte>(i),
                static_cast<std::byte>(i + 1),
            };
            Message out{MessageType::next_frame, std::move(payload)};
            neotape::tcp::write_message(fds[1], out);
        }

        for (int i = 0; i < 4; ++i) {
            auto in = neotape::tcp::read_message(fds[0]);
            if (!in.has_value()) {
                fail("expected sequential message");
            }
            if (in->type != MessageType::next_frame) {
                fail("wrong sequential message type");
            }
            if (in->payload.size() != 2) {
                fail("wrong sequential payload size");
            }
            if (static_cast<uint8_t>(in->payload[0]) !=
                static_cast<uint8_t>(i)) {
                fail("sequential payload[0] mismatch");
            }
            if (static_cast<uint8_t>(in->payload[1]) !=
                static_cast<uint8_t>(i + 1)) {
                fail("sequential payload[1] mismatch");
            }
        }
    }

    // ack_frame round-trip (8-byte little-endian uint64 payload).
    {
        uint64_t const seq = 0x123456789abcdef0u;
        std::vector<std::byte> payload;
        payload.reserve(8);
        for (std::size_t i = 0; i < 8; ++i) {
            payload.push_back(static_cast<std::byte>((seq >> (8 * i)) & 0xffu));
        }

        Message out{MessageType::ack_frame, std::move(payload)};
        neotape::tcp::write_message(fds[1], out);

        auto in = neotape::tcp::read_message(fds[0]);
        if (!in.has_value()) {
            fail("expected ack_frame message");
        }
        if (in->type != MessageType::ack_frame) {
            fail("wrong message type for ack_frame");
        }
        if (in->payload.size() != 8) {
            fail("wrong ack_frame payload size");
        }
        uint64_t decoded = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            decoded |=
                static_cast<uint64_t>(static_cast<uint8_t>(in->payload[i]))
                << (8 * i);
        }
        if (decoded != seq) {
            fail("ack_frame payload mismatch");
        }
    }

    // Clean close returns nullopt.
    close(fds[1]);
    auto end = neotape::tcp::read_message(fds[0]);
    if (end.has_value()) {
        fail("expected nullopt on clean close");
    }
    close(fds[0]);

    // --- Address parsing tests ---
    {
        // tcp://host:port
        auto a = neotape::tcp::parse_address("tcp://127.0.0.1:9123");
        if (a.is_unix)
            fail("tcp address should not be unix");
        if (a.host != "127.0.0.1")
            fail("tcp host mismatch: " + a.host);
        if (a.port != "9123")
            fail("tcp port mismatch: " + a.port);
    }
    {
        // tcp://ipv6 with brackets (recommended form)
        auto a = neotape::tcp::parse_address("tcp://[::1]:9123");
        if (a.is_unix)
            fail("ipv6 brackets: should not be unix");
        if (a.host != "[::1]")
            fail("ipv6 brackets host: " + a.host);
        if (a.port != "9123")
            fail("ipv6 brackets port: " + a.port);
    }
    {
        // tcp://ipv6 without brackets (rfind(':') on last colon)
        auto a = neotape::tcp::parse_address("tcp://::1:9123");
        if (a.is_unix)
            fail("ipv6 no brackets: should not be unix");
        if (a.host != "::1")
            fail("ipv6 no brackets host: " + a.host);
        if (a.port != "9123")
            fail("ipv6 no brackets port: " + a.port);
    }
    {
        // tcp:// wildcard bind
        auto a = neotape::tcp::parse_address("tcp://0.0.0.0:9123");
        if (a.is_unix)
            fail("wildcard: should not be unix");
        if (a.host != "0.0.0.0")
            fail("wildcard host: " + a.host);
        if (a.port != "9123")
            fail("wildcard port: " + a.port);
    }
    {
        // unix:// path
        auto a = neotape::tcp::parse_address("unix:///tmp/neotape.sock");
        if (!a.is_unix)
            fail("unix: should be unix");
        if (a.path != "/tmp/neotape.sock")
            fail("unix path: " + a.path);
    }
    {
        // Missing port throws
        bool threw = false;
        try {
            neotape::tcp::parse_address("tcp://localhost");
        } catch (const std::runtime_error &) {
            threw = true;
        }
        if (!threw)
            fail("missing port should throw");
    }
    {
        // Unknown prefix throws
        bool threw = false;
        try {
            neotape::tcp::parse_address("http://example.com:80");
        } catch (const std::runtime_error &) {
            threw = true;
        }
        if (!threw)
            fail("unknown prefix should throw");
    }
    {
        // Empty address throws
        bool threw = false;
        try {
            neotape::tcp::parse_address("");
        } catch (const std::runtime_error &) {
            threw = true;
        }
        if (!threw)
            fail("empty address should throw");
    }

    std::cout << "test_tcp_protocol: ok\n";
    return 0;
}
