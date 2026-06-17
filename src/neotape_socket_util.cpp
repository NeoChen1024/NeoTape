#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <memory>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace neotape {

using neotape::tcp::Address;
using neotape::tcp::Message;
using neotape::tcp::MessageType;
using neotape::tcp::parse_address;
using std::format;

int create_listener(const std::string &addr) {
    Address a = parse_address(addr);

    int fd = -1;
    if (a.is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error(
                format("socket: {}", std::strerror(errno)));
        }
        unlink(a.path.c_str());
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (a.path.size() >= sizeof(sa.sun_path)) {
            ::close(fd);
            throw std::runtime_error("unix socket path too long");
        }
        std::memcpy(sa.sun_path, a.path.data(), a.path.size());
        if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0) {
            ::close(fd);
            throw std::runtime_error(
                format("bind {}: {}", a.path, std::strerror(errno)));
        }
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *res = nullptr;
        int const gai =
            getaddrinfo(a.host.c_str(), a.port.c_str(), &hints, &res);
        if (gai != 0) {
            throw std::runtime_error(
                format("getaddrinfo: {}", gai_strerror(gai)));
        }
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> const res_guard(
            res, freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            throw std::runtime_error(
                format("socket: {}", std::strerror(errno)));
        }
        int yes = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
            ::close(fd);
            throw std::runtime_error(format("bind {}:{}: {}", a.host, a.port,
                                            std::strerror(errno)));
        }
    }

    if (listen(fd, 1) < 0) {
        ::close(fd);
        throw std::runtime_error(
            format("listen: {}", std::strerror(errno)));
    }
    return fd;
}

int connect_to_server(const std::string &addr) {
    Address a = parse_address(addr);

    int fd = -1;
    if (a.is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error(
                format("socket: {}", std::strerror(errno)));
        }
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (a.path.size() >= sizeof(sa.sun_path)) {
            ::close(fd);
            throw std::runtime_error("unix socket path too long");
        }
        std::memcpy(sa.sun_path, a.path.data(), a.path.size());
        if (connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0) {
            ::close(fd);
            throw std::runtime_error(
                format("connect {}: {}", a.path, std::strerror(errno)));
        }
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        int const gai =
            getaddrinfo(a.host.c_str(), a.port.c_str(), &hints, &res);
        if (gai != 0) {
            throw std::runtime_error(
                format("getaddrinfo: {}", gai_strerror(gai)));
        }
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> const res_guard(
            res, freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            throw std::runtime_error(
                format("socket: {}", std::strerror(errno)));
        }
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
            ::close(fd);
            throw std::runtime_error(format("connect {}:{}: {}", a.host,
                                            a.port, std::strerror(errno)));
        }
    }
    return fd;
}

void send_error(int client, const char *text) {
    auto payload = std::vector<std::byte>(
        reinterpret_cast<const std::byte *>(text),
        reinterpret_cast<const std::byte *>(text) + std::strlen(text));
    neotape::tcp::write_message(
        client, Message{MessageType::error, std::move(payload)});
}

std::vector<std::byte> uint64_to_le_bytes(uint64_t v) {
    std::vector<std::byte> out(8);
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xffU);
    }
    return out;
}

uint64_t le64_from_bytes(const std::vector<std::byte> &payload) {
    uint64_t v = 0;
    for (std::size_t i = 0; i < payload.size() && i < 8; ++i) {
        v |= static_cast<uint64_t>(static_cast<uint8_t>(payload[i])) << (8 * i);
    }
    return v;
}

} // namespace neotape
