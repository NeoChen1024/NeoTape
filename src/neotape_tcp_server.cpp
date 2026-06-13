#include "neotape/tcp_server.hpp"
#include "neotape/format.hpp"
#include "neotape/tcp_protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace neotape {

namespace {

using neotape::tcp::Message;
using neotape::tcp::MessageType;

int parse_listen_address(const std::string &addr, std::string &host,
                         std::string &port, std::string &path, bool &is_unix) {
    const std::string tcp_prefix = "tcp://";
    const std::string unix_prefix = "unix://";
    if (addr.rfind(tcp_prefix, 0) == 0) {
        is_unix = false;
        std::string rest = addr.substr(tcp_prefix.size());
        auto colon = rest.rfind(':');
        if (colon == std::string::npos)
            throw std::runtime_error("tcp listen address missing port");
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        return 0;
    }
    if (addr.rfind(unix_prefix, 0) == 0) {
        is_unix = true;
        path = addr.substr(unix_prefix.size());
        return 0;
    }
    throw std::runtime_error(
        "listen address must start with tcp:// or unix://");
}

int create_listener(const std::string &addr) {
    std::string host, port, path;
    bool is_unix = false;
    parse_listen_address(addr, host, port, path, is_unix);

    int fd = -1;
    if (is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error(
                std::format("socket: {}", std::strerror(errno)));
        unlink(path.c_str());
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (path.size() >= sizeof(sa.sun_path))
            throw std::runtime_error("unix socket path too long");
        std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0)
            throw std::runtime_error(
                std::format("bind {}: {}", path, std::strerror(errno)));
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *res = nullptr;
        int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
        if (gai != 0)
            throw std::runtime_error(
                std::format("getaddrinfo: {}", gai_strerror(gai)));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res_guard(res,
                                                                     freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0)
            throw std::runtime_error(
                std::format("socket: {}", std::strerror(errno)));
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, res->ai_addr, res->ai_addrlen) < 0)
            throw std::runtime_error(
                std::format("bind {}:{}: {}", host, port, std::strerror(errno)));
    }

    if (listen(fd, 1) < 0)
        throw std::runtime_error(
            std::format("listen: {}", std::strerror(errno)));
    return fd;
}

VolumeHeader make_volume_header(uint32_t block_size, uint64_t volume_seq_num,
                                const std::string &archive_name) {
    VolumeHeader vh;
    vh.volume_block_size = block_size;
    vh.archive_uuid = make_uuid_v4();
    vh.archive_name = archive_name;
    vh.volume_seq_num = volume_seq_num;
    vh.payload_profile = PayloadProfile::pax;
    vh.volume_write_at_utc = utc_timestamp_now();
    vh.flags = 0;
    return vh;
}

std::vector<std::byte> bytes_from_header_bytes(const HeaderBytes &bytes) {
    std::vector<std::byte> out;
    out.reserve(bytes.size());
    for (uint8_t b : bytes)
        out.push_back(static_cast<std::byte>(b));
    return out;
}

} // namespace

uint64_t run_tcp_archiver(const TcpArchiverOptions &opts) {
    if (!valid_block_size(opts.volume_block_size))
        throw std::runtime_error("invalid volume block size");

    int listener = create_listener(opts.listen_address);
    std::cerr << std::format("archiver listening on {}\n", opts.listen_address);

    int client = accept(listener, nullptr, nullptr);
    if (client < 0)
        throw std::runtime_error(
            std::format("accept: {}", std::strerror(errno)));
    close(listener);

    uint64_t frames_served = 0;
    try {
        VolumeHeader vh = make_volume_header(opts.volume_block_size,
                                             opts.initial_volume_seq_num,
                                             opts.archive_name);
        HeaderBytes vh_bytes = serialize_volume_header(vh);
        std::vector<std::byte> vh_payload = bytes_from_header_bytes(vh_bytes);

        for (;;) {
            auto req = neotape::tcp::read_message(client);
            if (!req.has_value())
                break;

            switch (req->type) {
            case MessageType::get_volume_header:
                neotape::tcp::write_message(
                    client, Message{MessageType::volume_header,
                                    std::move(vh_payload)});
                vh_payload = bytes_from_header_bytes(vh_bytes);
                break;
            case MessageType::next_frame:
                if (!opts.has_more_frames(frames_served)) {
                    ArchiveEndHeader ae;
                    ae.volume_block_size = opts.volume_block_size;
                    ae.archive_uuid = vh.archive_uuid;
                    ae.archive_name = opts.archive_name;
                    ae.volume_seq_num = opts.initial_volume_seq_num;
                    ae.payload_profile = PayloadProfile::pax;
                    ae.last_logical_slice_seq_num = 0;
                    ae.last_global_frame_seq_num = frames_served;
                    ae.created_by_implementation = "neotape-archiver";
                    ae.created_by_build_id = "";
                    ae.archive_end_at_utc = utc_timestamp_now();
                    ae.flags = archive_end_flag_clean_end;
                    HeaderBytes ae_bytes = serialize_archive_end_header(ae);
                    neotape::tcp::write_message(
                        client,
                        Message{MessageType::archive_end_header,
                                bytes_from_header_bytes(ae_bytes)});
                    close(client);
                    return frames_served;
                }
                if (frames_served > 0 && frames_served % 4 == 0) {
                    neotape::tcp::write_message(
                        client, Message{MessageType::tape_eof, {}});
                } else {
                    auto rec = opts.produce_record(frames_served);
                    if (rec.size() != opts.volume_block_size)
                        throw std::runtime_error("produce_record size mismatch");
                    neotape::tcp::write_message(
                        client,
                        Message{MessageType::frame_record, std::move(rec)});
                    ++frames_served;
                }
                break;
            default:
                neotape::tcp::write_message(
                    client,
                    Message{MessageType::error,
                            std::vector<std::byte>{}});
                close(client);
                return frames_served;
            }
        }
    } catch (...) {
        close(client);
        throw;
    }
    close(client);
    return frames_served;
}

} // namespace neotape
