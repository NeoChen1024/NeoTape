#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neotape::tcp {

enum class MessageType : uint8_t {
    get_volume_header = 0x01,
    volume_header = 0x02,
    next_frame = 0x03,
    frame_record = 0x04,
    archive_end_header = 0x05,
    tape_eof = 0x06,
    error = 0x07,
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

} // namespace neotape::tcp
