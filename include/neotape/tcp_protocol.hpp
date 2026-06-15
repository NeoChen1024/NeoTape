#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neotape::tcp {

enum class MessageType : uint8_t {
    next_frame = 0x01,
    frame_record = 0x02,
    tape_eof = 0x03,
    error = 0x04,
    // ack_frame payload: uint64_t little-endian global frame sequence number.
    ack_frame = 0x05,
};

struct Message {
    MessageType type;
    std::vector<std::byte> payload;

    Message() = default;
    Message(MessageType t, std::vector<std::byte> p = {})
        : type(t), payload(std::move(p)) {}
};

constexpr std::size_t message_header_size = 1 + 8; // type (1 byte) + little-endian length (8 bytes)

// Throws std::runtime_error on I/O or protocol errors.
// Returns std::nullopt if the peer closed cleanly before any message.
std::optional<Message> read_message(int fd);

// Throws std::runtime_error on I/O errors.
void write_message(int fd, const Message &msg);

std::string message_type_name(MessageType type);

struct Address {
    bool is_unix = false;
    std::string host; // tcp host
    std::string port; // tcp port
    std::string path; // unix path
};

// Parse a tcp://host:port or unix://path address string.
// Throws std::runtime_error on invalid input.
Address parse_address(const std::string &addr);

} // namespace neotape::tcp
