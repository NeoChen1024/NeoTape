#include "neotape/common.hpp"
#include "neotape/tape.hpp"
#include "neotape/tcp_protocol.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <string>
#include <variant>
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

struct TargetLocator {
    enum Kind { none, tape, spool } kind = none;
    std::string path;
};

TargetLocator parse_target(const std::string &s) {
    if (s.rfind("tape:", 0) == 0)
        return {TargetLocator::tape, s.substr(5)};
    if (s.rfind("spool:", 0) == 0)
        return {TargetLocator::spool, s.substr(6)};
    throw std::runtime_error(
        "target must be tape:<device> or spool:<dir>");
}

struct Options {
    string source_address;
    string output_path = "-";
    TargetLocator target;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --source <tcp://host:port|unix://path>\n"
        "       [--target <tape:/dev/nst0|spool:./dir> | -o <file|->]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"output", required_argument, nullptr, 'o'},
        {"target", required_argument, nullptr, 256},
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
        case 256:
            opts.target = parse_target(optarg);
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
    if (opts.target.kind != TargetLocator::none &&
        opts.output_path != "-") {
        fail("--target and -o are mutually exclusive");
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

        FdGuard fd_guard(connect_to_source(opts.source_address));
        int fd = fd_guard.fd;

        using neotape::tcp::Message;
        using neotape::tcp::MessageType;

        neotape::tcp::write_message(fd, Message{MessageType::get_volume_header});
        auto vh = neotape::tcp::read_message(fd);
        if (!vh || vh->type != MessageType::volume_header)
            fail("did not receive volume header");

        // Output abstraction: either a raw file/stream or a tape/spool device.
        struct FileOutput {
            FILE *file = nullptr;
            bool owned = false;
        };
        struct TargetOutput {
            std::unique_ptr<mt::TapeDevice> device;
        };
        std::variant<FileOutput, TargetOutput> output;

        if (opts.target.kind == TargetLocator::none) {
            FILE *raw_out = nullptr;
            bool owned = false;
            if (opts.output_path == "-") {
                raw_out = stdout;
            } else {
                raw_out = std::fopen(opts.output_path.c_str(), "wb");
                if (!raw_out)
                    fail(format("open {}: {}", opts.output_path,
                                std::strerror(errno)));
                owned = true;
            }
            output = FileOutput{raw_out, owned};
        } else {
            std::unique_ptr<mt::TapeDevice> dev;
            if (opts.target.kind == TargetLocator::tape) {
                dev = std::make_unique<mt::TapeDevice>(opts.target.path, true);
            } else {
                dev = std::make_unique<mt::SpoolTapeDevice>(
                    std::filesystem::path(opts.target.path), true);
            }
            dev->rewind();
            output = TargetOutput{std::move(dev)};
        }

        auto write_bytes = [&](const std::vector<std::byte> &bytes) {
            if (std::holds_alternative<FileOutput>(output)) {
                FILE *f = std::get<FileOutput>(output).file;
                if (std::fwrite(bytes.data(), 1, bytes.size(), f) !=
                    bytes.size())
                    fail("write output");
            } else {
                mt::TapeDevice *dev = std::get<TargetOutput>(output).device.get();
                if (dev->status().eot())
                    throw std::runtime_error("end of tape");
                dev->write_record(bytes.data(), bytes.size());
            }
        };

        auto write_filemark = [&]() {
            if (std::holds_alternative<TargetOutput>(output))
                std::get<TargetOutput>(output).device->write_filemark();
        };

        auto close_output = [&]() {
            if (std::holds_alternative<FileOutput>(output)) {
                FileOutput &fo = std::get<FileOutput>(output);
                if (fo.owned && fo.file)
                    std::fclose(fo.file);
                fo.file = nullptr;
                fo.owned = false;
            }
        };

        write_bytes(vh->payload);

        uint64_t frames = 0;
        bool eot_reached = false;
        for (;;) {
            neotape::tcp::write_message(fd, Message{MessageType::next_frame});
            auto msg = neotape::tcp::read_message(fd);
            if (!msg)
                fail("unexpected disconnect");
            switch (msg->type) {
            case MessageType::frame_record:
                try {
                    write_bytes(msg->payload);
                } catch (const std::runtime_error &e) {
                    if (std::string(e.what()) == "end of tape") {
                        write_filemark();
                        eot_reached = true;
                    } else {
                        throw;
                    }
                }
                if (eot_reached) {
                    close_output();
                    std::cerr << format(
                        "writer: reached end of tape after {} frames\n", frames);
                    return 1;
                }
                ++frames;
                break;
            case MessageType::tape_eof:
                write_filemark();
                break;
            case MessageType::archive_end_header:
                write_bytes(msg->payload);
                close_output();
                std::cerr << format(
                    "writer: received archive end after {} frames\n", frames);
                return 0;
            case MessageType::error: {
                string reason(msg->payload.begin(), msg->payload.end());
                if (reason.empty())
                    reason = "archiver reported error";
                fail(reason);
            }
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
