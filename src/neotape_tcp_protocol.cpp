#include "neotape/tcp_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>
#include <unistd.h>

namespace neotape::tcp {

namespace {

std::size_t read_exact(int fd, void *buf, std::size_t n) {
    auto *p = static_cast<std::byte *>(buf);
    std::size_t remaining = n;
    while (remaining > 0) {
        ssize_t const r = ::read(fd, p, remaining);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                std::format("read failed: {}", std::strerror(errno)));
        }
        if (r == 0) {
            break;
        }
        p += r;
        remaining -= static_cast<std::size_t>(r);
    }
    return n - remaining;
}

void write_exact(int fd, const void *buf, std::size_t n) {
    const auto *p = static_cast<const std::byte *>(buf);
    std::size_t remaining = n;
    while (remaining > 0) {
        ssize_t const w = ::write(fd, p, remaining);
        if (w == 0) {
            throw std::runtime_error("write returned 0 bytes");
        }
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                std::format("write failed: {}", std::strerror(errno)));
        }
        p += w;
        remaining -= static_cast<std::size_t>(w);
    }
}

} // namespace

// valid message types: 0x01 through 0x05
constexpr bool is_valid_message_type(uint8_t const b) {
    return b >= 0x01 && b <= 0x05;
}

std::optional<Message> read_message(int fd) {
    std::byte header[message_header_size];
    std::size_t got = read_exact(fd, header, message_header_size);
    if (got == 0) {
        return std::nullopt;
    }
    if (got != message_header_size) {
        throw std::runtime_error("short read on message header");
    }

    const uint8_t type_byte = static_cast<uint8_t>(header[0]);
    if (!is_valid_message_type(type_byte)) {
        throw std::runtime_error(
            std::format("unknown message type 0x{:02x}", type_byte));
    }

    uint64_t length = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        length |= static_cast<uint64_t>(static_cast<uint8_t>(header[1 + i]))
                  << (8 * i);
    }

    if (length > max_message_payload_size) {
        throw std::runtime_error(
            std::format("message payload {} exceeds maximum {}", length,
                        max_message_payload_size));
    }

    auto const type = static_cast<MessageType>(type_byte);
    Message msg{type, {}};
    if (length > 0) {
        msg.payload.resize(length);
        got = read_exact(fd, msg.payload.data(), length);
        if (got != length) {
            throw std::runtime_error("short read on message payload");
        }
    }
    return msg;
}

void write_message(int fd, const Message &msg) {
    std::byte header[message_header_size];
    header[0] = static_cast<std::byte>(static_cast<uint8_t>(msg.type));
    uint64_t const length = msg.payload.size();
    for (std::size_t i = 0; i < 8; ++i) {
        header[1 + i] = static_cast<std::byte>(
            static_cast<uint8_t>((length >> (8 * i)) & 0xffU));
    }

    write_exact(fd, header, message_header_size);
    if (length > 0) {
        write_exact(fd, msg.payload.data(), length);
    }
}

std::string message_type_name(MessageType type) {
    switch (type) {
    case MessageType::next_frame:
        return "NEXT_FRAME";
    case MessageType::frame_record:
        return "FRAME_RECORD";
    case MessageType::tape_eof:
        return "TAPE_EOF";
    case MessageType::error:
        return "ERROR";
    case MessageType::ack_frame:
        return "ACK_FRAME";
    }
    return std::format("UNKNOWN({})", static_cast<int>(type));
}

Address parse_address(const std::string &addr) {
    Address result;
    const std::string tcp_prefix = "tcp://";
    const std::string unix_prefix = "unix://";
    if (addr.starts_with(tcp_prefix)) {
        result.is_unix = false;
        std::string const rest = addr.substr(tcp_prefix.size());
        auto colon = rest.rfind(':');
        if (colon == std::string::npos) {
            throw std::runtime_error("tcp address missing port");
        }
        result.host = rest.substr(0, colon);
        result.port = rest.substr(colon + 1);
        return result;
    }
    if (addr.starts_with(unix_prefix)) {
        result.is_unix = true;
        result.path = addr.substr(unix_prefix.size());
        return result;
    }
    throw std::runtime_error("address must start with tcp:// or unix://");
}

} // namespace neotape::tcp
