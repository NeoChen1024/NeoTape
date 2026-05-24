#include "neotape/format.hpp"
#include "neotape/tape.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
using std::format;
using std::string;

constexpr std::size_t tape_probe_read_size = 8 * 1024 * 1024;

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-init: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage(const char *prog) {
    std::cerr << format("usage: {} -f <device> [--label <text>] [--force]\n",
                        prog);
    std::exit(2);
}

struct Options {
    string device;
    string label;
    bool force = false;
};

Options parse_args(int argc, char **argv) {
    static const option long_opts[] = {
        {"label", required_argument, nullptr, 'l'},
        {"force", no_argument, nullptr, 'F'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "f:l:Fh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'f':
            opts.device = optarg;
            break;
        case 'l':
            opts.label = optarg;
            break;
        case 'F':
            opts.force = true;
            break;
        case 'h':
            usage(argv[0]);
            break;
        case '?':
            std::exit(2);
        }
    }

    if (opts.device.empty()) {
        const char *env = std::getenv("TAPE");
        if (env)
            opts.device = env;
    }
    if (opts.device.empty())
        fail("no tape device specified (use -f or $TAPE)");

    return opts;
}

} // namespace

int neotape_init_main(int argc, char **argv) {
    try {
        auto opts = parse_args(argc, argv);

        mt::TapeDevice dev(opts.device, true);
        dev.configure_preferred_variable_block_mode(
            65536, "neotape-init medium header", std::cerr);

        // Check if already initialized
        dev.rewind();
        std::vector<uint8_t> buf(tape_probe_read_size);
        ssize_t n = ::read(dev.fd(), buf.data(), buf.size());
        if (n > 0)
            buf.resize(static_cast<std::size_t>(n));
        if (n > 0 && buf.size() >= neotape::fixed_header_size) {
            try {
                auto parsed =
                    neotape::parse_fixed_header(buf.data(), buf.size());
                if (parsed.type == neotape::HeaderType::medium && !opts.force)
                    fail("medium already initialized (use --force to "
                         "overwrite)");
            } catch (const std::exception &) {
                // Not a valid NeoTape header — proceed with init
            }
        }

        // Write Medium Header
        neotape::MediumHeader mh;
        mh.medium_uuid = neotape::make_uuid_v4();
        mh.medium_label = opts.label;
        mh.initialized_at_utc = neotape::utc_timestamp_now();
        mh.medium_header_block_size = 65536;
        mh.medium_header_block_count = 1;
        mh.created_by_implementation = "NeoTape init phase6-mvp";

        auto bytes = neotape::serialize_medium_header(mh);

        // Pad to block size and write
        std::vector<uint8_t> record(mh.medium_header_block_size, 0);
        std::memcpy(record.data(), bytes.data(), bytes.size());

        n = ::write(dev.fd(), record.data(), record.size());
        if (n < 0)
            fail(format("write medium header: {}", std::strerror(errno)));
        if (static_cast<std::size_t>(n) != record.size())
            fail("short write on medium header");

        dev.write_filemark();
        std::cerr << format("medium initialized: uuid={}\n", mh.medium_uuid);
        return 0;

    } catch (const std::exception &e) {
        fail(e.what());
    }
}

#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) { return neotape_init_main(argc, argv); }
#endif
