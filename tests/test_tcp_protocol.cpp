#include "neotape/tcp_protocol.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

using neotape::tcp::Message;
using neotape::tcp::MessageType;

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "test_tcp_protocol: " << msg << "\n";
    std::exit(1);
}

int main() {
    int fds[2];
    if (pipe(fds) != 0)
        fail("pipe failed");

    std::vector<std::byte> payload;
    payload.reserve(256);
    for (int i = 0; i < 256; ++i)
        payload.push_back(static_cast<std::byte>(i));

    Message out{MessageType::frame_record, std::move(payload)};
    neotape::tcp::write_message(fds[1], out);

    auto in = neotape::tcp::read_message(fds[0]);
    if (!in.has_value())
        fail("expected a message");
    if (in->type != MessageType::frame_record)
        fail("wrong message type");
    if (in->payload.size() != 256)
        fail("wrong payload size");
    for (int i = 0; i < 256; ++i) {
        if (static_cast<uint8_t>(in->payload[i]) != static_cast<uint8_t>(i))
            fail("payload mismatch");
    }

    // Clean close returns nullopt.
    close(fds[1]);
    auto end = neotape::tcp::read_message(fds[0]);
    if (end.has_value())
        fail("expected nullopt on clean close");
    close(fds[0]);

    std::cout << "test_tcp_protocol: ok\n";
    return 0;
}
