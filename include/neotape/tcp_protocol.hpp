#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neotape::tcp {

//
// Message roles (Server ↔ Client):
//
//   next_frame   — Server requests next frame          (dir: Server → Client)
//   frame_record — Client sends frame bytes            (dir: Client → Server)
//   tape_eof     — Client signals no more frames       (dir: Client → Server)
//   error        — Either side reports an error        (dir: bidirectional)
//   ack_frame    — Server confirms frame was validated  (dir: Server → Client)
//                  payload: uint64_t little-endian global frame seq num
//
// In the archiver pipeline Server = Archiver, Client = Writer.
// In the extractor pipeline Server = Extractor, Client = Reader.
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
