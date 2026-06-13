#include "neotape/common.hpp"
#include "neotape/tcp_server.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <getopt.h>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using std::format;
using std::string;
using std::vector;

struct Options {
    string listen_address;
    uint32_t volume_block_size = 4 * 1024 * 1024;
    string archive_name = "archive";
    uint64_t dummy_frame_count = 8;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-archiver: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --listen <tcp://host:port|unix://path>\n"
        "       [--volume-block-size <bytes>] [--archive-name <name>]\n"
        "       [--dummy-frame-count <N>]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"archive-name", required_argument, nullptr, 'n'},
        {"dummy-frame-count", required_argument, nullptr, 'd'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "l:b:n:d:h", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 'l':
            opts.listen_address = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 'd': {
            char *end = nullptr;
            unsigned long n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0')
                fail("--dummy-frame-count requires a number");
            opts.dummy_frame_count = n;
            break;
        }
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.listen_address.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    return opts;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            fail("failed to ignore SIGPIPE");

        neotape::TcpArchiverOptions server_opts;
        server_opts.listen_address = opts.listen_address;
        server_opts.volume_block_size = opts.volume_block_size;
        server_opts.archive_name = opts.archive_name;
        server_opts.has_more_frames = [n = opts.dummy_frame_count](
                                          uint64_t idx) { return idx < n; };
        server_opts.produce_record = [size = opts.volume_block_size](
                                         uint64_t idx) {
            vector<std::byte> rec(size);
            // Fill with a recognizable pattern.
            for (uint32_t i = 0; i < size; ++i)
                rec[i] = static_cast<std::byte>(static_cast<uint8_t>(idx + i));
            return rec;
        };

        uint64_t served = neotape::run_tcp_archiver(server_opts);
        std::cerr << format("archiver served {} frames\n", served);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
