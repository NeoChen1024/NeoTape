#include "neotape/common.hpp"
#include "neotape/tcp_protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <getopt.h>
#include <iostream>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

using std::format;
using std::string;
using std::vector;

struct Options {
    string source_address;
    string output_path = "-";
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --source <tcp://host:port|unix://path> [-o <file|->]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"output", required_argument, nullptr, 'o'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "s:o:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source_address = optarg;
            break;
        case 'o':
            opts.output_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.source_address.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    return opts;
}

void parse_source_address(const string &addr, string &host, string &port,
                          string &path, bool &is_unix) {
    const string tcp_prefix = "tcp://";
    const string unix_prefix = "unix://";
    if (addr.rfind(tcp_prefix, 0) == 0) {
        is_unix = false;
        string rest = addr.substr(tcp_prefix.size());
        auto colon = rest.rfind(':');
        if (colon == std::string::npos)
            fail("tcp source address missing port");
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        return;
    }
    if (addr.rfind(unix_prefix, 0) == 0) {
        is_unix = true;
        path = addr.substr(unix_prefix.size());
        return;
    }
    fail("source address must start with tcp:// or unix://");
}

int connect_to_source(const string &addr) {
    string host, port, path;
    bool is_unix = false;
    parse_source_address(addr, host, port, path, is_unix);

    int fd = -1;
    if (is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            fail(format("socket: {}", std::strerror(errno)));
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (path.size() >= sizeof(sa.sun_path))
            fail("unix socket path too long");
        std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0)
            fail(format("connect {}: {}", path, std::strerror(errno)));
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
        if (gai != 0)
            fail(format("getaddrinfo: {}", gai_strerror(gai)));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res_guard(res,
                                                                     freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0)
            fail(format("socket: {}", std::strerror(errno)));
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
            fail(format("connect {}:{}: {}", host, port, std::strerror(errno)));
    }
    return fd;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        FILE *out = nullptr;
        bool close_out = false;
        if (opts.output_path == "-") {
            out = stdout;
        } else {
            out = std::fopen(opts.output_path.c_str(), "wb");
            if (!out)
                fail(format("open {}: {}", opts.output_path,
                            std::strerror(errno)));
            close_out = true;
        }

        int fd = connect_to_source(opts.source_address);

        using neotape::tcp::Message;
        using neotape::tcp::MessageType;

        neotape::tcp::write_message(fd, Message{MessageType::get_volume_header});
        auto vh = neotape::tcp::read_message(fd);
        if (!vh || vh->type != MessageType::volume_header)
            fail("did not receive volume header");
        if (std::fwrite(vh->payload.data(), 1, vh->payload.size(), out) !=
            vh->payload.size())
            fail("write output");

        uint64_t frames = 0;
        for (;;) {
            neotape::tcp::write_message(fd, Message{MessageType::next_frame});
            auto msg = neotape::tcp::read_message(fd);
            if (!msg)
                fail("unexpected disconnect");
            switch (msg->type) {
            case MessageType::frame_record:
                if (std::fwrite(msg->payload.data(), 1, msg->payload.size(),
                                out) != msg->payload.size())
                    fail("write output");
                ++frames;
                break;
            case MessageType::tape_eof:
                // In skeleton mode just print a marker to stderr.
                std::cerr << "writer: tape eof marker\n";
                break;
            case MessageType::archive_end_header:
                if (std::fwrite(msg->payload.data(), 1, msg->payload.size(),
                                out) != msg->payload.size())
                    fail("write output");
                std::cerr << format("writer: received archive end after {} frames\n",
                                    frames);
                close(fd);
                if (close_out)
                    std::fclose(out);
                return 0;
            case MessageType::error:
                fail("archiver reported error");
            default:
                fail(format("unexpected message type {}",
                            static_cast<int>(msg->type)));
            }
        }
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
