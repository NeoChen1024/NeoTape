#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace neotape {

// RAII file descriptor guard.
struct FdGuard {
    int fd = -1;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
    FdGuard(FdGuard &&) = delete;
    FdGuard &operator=(FdGuard &&) = delete;
};

// Create a listening socket for a tcp://host:port or unix://path address.
// Throws std::runtime_error on failure.
int create_listener(const std::string &addr);

// Connect to a tcp://host:port or unix://path address.
// Throws std::runtime_error on failure.  Returns the connected fd.
int connect_to_server(const std::string &addr);

// Send an error message over a TCP/UDS connection.
void send_error(int client, const char *text);

// The text of a received error message; `fallback` when it is empty.
std::string error_payload_text(const std::vector<std::byte> &payload,
                               std::string_view fallback);

// Raw bytes as a message payload.
std::vector<std::byte> bytes_payload(std::span<const uint8_t> bytes);

// Encode a uint64 as 8 little-endian bytes.
std::vector<std::byte> uint64_to_le_bytes(uint64_t v);

// Decode up to 8 little-endian bytes into a uint64.
uint64_t le64_from_bytes(const std::vector<std::byte> &payload);

} // namespace neotape
