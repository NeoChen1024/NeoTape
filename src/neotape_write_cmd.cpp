#include "neotape/common.hpp"
#include "neotape/tcp_protocol.hpp"

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

using neotape::tcp::Address;
using neotape::tcp::parse_address;
using std::format;
using std::string;
using std::vector;

struct FdGuard {
    int fd = -1;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() {
        if (fd >= 0)
            ::close(fd);
    }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
};

struct FileGuard {
    FILE *file = nullptr;
    bool owned = false;
    FileGuard(FILE *f, bool own) : file(f), owned(own) {}
    ~FileGuard() {
        if (owned && file)
            std::fclose(file);
    }
    FileGuard(const FileGuard &) = delete;
    FileGuard &operator=(const FileGuard &) = delete;
};

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

int connect_to_source(const string &addr) {
    Address a = parse_address(addr);

    int fd = -1;
    if (a.is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            fail(format("socket: {}", std::strerror(errno)));
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (a.path.size() >= sizeof(sa.sun_path))
            fail("unix socket path too long");
        std::memcpy(sa.sun_path, a.path.data(), a.path.size());
        if (connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0)
            fail(format("connect {}: {}", a.path, std::strerror(errno)));
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        int gai = getaddrinfo(a.host.c_str(), a.port.c_str(), &hints, &res);
        if (gai != 0)
            fail(format("getaddrinfo: {}", gai_strerror(gai)));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res_guard(res,
                                                                     freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0)
            fail(format("socket: {}", std::strerror(errno)));
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
            fail(format("connect {}:{}: {}", a.host, a.port, std::strerror(errno)));
    }
    return fd;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        FILE *raw_out = nullptr;
        bool close_out = false;
        if (opts.output_path == "-") {
            raw_out = stdout;
        } else {
            raw_out = std::fopen(opts.output_path.c_str(), "wb");
            if (!raw_out)
                fail(format("open {}: {}", opts.output_path,
                            std::strerror(errno)));
            close_out = true;
        }
        FileGuard out_guard(raw_out, close_out);

        FdGuard fd_guard(connect_to_source(opts.source_address));
        int fd = fd_guard.fd;

        using neotape::tcp::Message;
        using neotape::tcp::MessageType;

        neotape::tcp::write_message(fd, Message{MessageType::get_volume_header});
        auto vh = neotape::tcp::read_message(fd);
        if (!vh || vh->type != MessageType::volume_header)
            fail("did not receive volume header");
        if (std::fwrite(vh->payload.data(), 1, vh->payload.size(), raw_out) !=
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
                                raw_out) != msg->payload.size())
                    fail("write output");
                ++frames;
                break;
            case MessageType::tape_eof:
                // In skeleton mode just print a marker to stderr.
                std::cerr << "writer: tape eof marker\n";
                break;
            case MessageType::archive_end_header:
                if (std::fwrite(msg->payload.data(), 1, msg->payload.size(),
                                raw_out) != msg->payload.size())
                    fail("write output");
                std::cerr << format("writer: received archive end after {} frames\n",
                                    frames);
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
