#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neotape::tcp {

//
// Message roles — direction depends on the pipeline:
//
//   Writing pipeline (Archiver = Server, Writer = Client):
//     next_frame   — Writer requests next frame            (Client → Server)
//     frame_record — Archiver sends frame bytes            (Server → Client)
//     tape_eof     — Archiver signals slice boundary       (Server → Client)
//     ack_frame    — Writer confirms frame was written     (Client → Server)
//
//   Reading pipeline (Extractor = Server, Reader = Client):
//     next_frame   — Extractor requests next frame         (Server → Client)
//     frame_record — Reader sends frame bytes              (Client → Server)
//     tape_eof     — Reader signals physical EOT           (Client → Server)
//     ack_frame    — Extractor confirms frame validated    (Server → Client)
//
//   error — always bidirectional.
//

enum class MessageType : uint8_t {
    next_frame = 0x01,
    frame_record = 0x02,
    tape_eof = 0x03,
    error = 0x04,
    ack_frame = 0x05,
};

struct Message {
    MessageType type;
    std::vector<std::byte> payload;

    Message() = default;
    Message(MessageType t, std::vector<std::byte> p = {})
        : type(t), payload(std::move(p)) {}
};

constexpr std::size_t message_header_size =
    1 + 8; // type (1 byte) + little-endian length (8 bytes)

// Maximum payload size accepted by read_message().  Set to twice the
// maximum NeoTape record size (2 × 8 MiB) to leave room for future
// extensions while preventing allocation from a corrupt length field.
constexpr std::size_t max_message_payload_size = 16 * 1024 * 1024;

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
//
// Supported forms:
//   tcp://host:port          IPv4 hostname or address
//   tcp://[ipv6]:port        IPv6 address (brackets recommended)
//   tcp://ipv6%iface:port    IPv6 with zone id (no brackets)
//   unix://path              absolute Unix-domain path (e.g. unix:///tmp/sock)
//
// Limitations: the parser splits on the last colon with rfind(':'). IPv6
// addresses without brackets are accepted as long as the last colon
// separates address from port.  Wildcard bindings use "0.0.0.0" (IPv4)
// or "[::]" (IPv6) as the host.
Address parse_address(const std::string &addr);

} // namespace neotape::tcp
