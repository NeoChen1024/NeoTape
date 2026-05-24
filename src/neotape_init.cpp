#include "neotape/cli.hpp"
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/tape.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <nlohmann-json/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
using std::format;
using std::string;

constexpr std::size_t tape_probe_read_size = 8 * 1024 * 1024;

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-init: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage(const char *prog) {
    std::cerr << format(
        "usage: {} <tape:<device>|spool:<dir>> [--label <text>] [--force]\n"
        "       {} spool:<dir> [--label <text>] [--force] "
        "[--virtual-tape-size <bytes>]\n",
        prog, prog);
    std::exit(2);
}

struct Options {
    neotape::Locator target;
    string label;
    bool force = false;
    uint64_t virtual_tape_size = 0;
};

Options parse_args(int argc, char **argv) {
    static const option long_opts[] = {
        {"label", required_argument, nullptr, 'l'},
        {"force", no_argument, nullptr, 'F'},
        {"virtual-tape-size", required_argument, nullptr, 256},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "l:Fh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'l':
            opts.label = optarg;
            break;
        case 'F':
            opts.force = true;
            break;
        case 256:
            opts.virtual_tape_size =
                neotape::parse_size(optarg, "virtual tape size");
            break;
        case 'h':
            usage(argv[0]);
            break;
        case '?':
            std::exit(2);
        }
    }

    if (optind >= argc)
        usage(argv[0]);
    opts.target = neotape::parse_locator(argv[optind++]);
    if (optind != argc)
        usage(argv[0]);
    if (opts.target.kind != "tape" && opts.target.kind != "spool")
        fail("init target must be tape: or spool:");
    if (opts.target.kind == "tape" && opts.virtual_tape_size != 0)
        fail("--virtual-tape-size is only valid for spool targets");

    return opts;
}

void init_spool(const Options &opts) {
    fs::path root(opts.target.locator);
    std::error_code ec;

    if (fs::exists(root, ec)) {
        if (!fs::is_directory(root, ec))
            fail(format("spool target is not a directory: {}", root.string()));
        if (!fs::is_empty(root, ec)) {
            if (!opts.force)
                fail(format("spool target is non-empty: {}", root.string()));
            fs::remove_all(root, ec);
            if (ec)
                fail(format("remove {}: {}", root.string(), ec.message()));
            fs::create_directories(root, ec);
        }
    } else {
        fs::create_directories(root, ec);
    }
    if (ec)
        fail(format("create {}: {}", root.string(), ec.message()));

    nlohmann::json manifest;
    manifest["kind"] = "neotape-spool-medium";
    manifest["label"] = opts.label;
    manifest["virtual_tape_size"] = opts.virtual_tape_size;
    manifest["volumes"] = nlohmann::json::array();

    std::ofstream out(root / "manifest.json", std::ios::binary);
    if (!out)
        fail(format("open manifest: {}", std::strerror(errno)));
    out << manifest.dump(2) << "\n";

    mt::SpoolTapeDevice dev(root, true);
    dev.set_block_size(65536);
    neotape::MediumHeader mh;
    mh.medium_uuid = neotape::make_uuid_v4();
    mh.medium_label = opts.label;
    mh.initialized_at_utc = neotape::utc_timestamp_now();
    mh.medium_header_block_size = 65536;
    mh.medium_header_block_count = 1;
    mh.created_by_implementation = "NeoTape init phase6-mvp";

    auto bytes = neotape::serialize_medium_header(mh);
    std::vector<uint8_t> record(mh.medium_header_block_size, 0);
    std::memcpy(record.data(), bytes.data(), bytes.size());
    ssize_t n = ::write(dev.fd(), record.data(), record.size());
    if (n < 0)
        fail(format("write medium header: {}", std::strerror(errno)));
    if (static_cast<std::size_t>(n) != record.size())
        fail("short write on medium header");
    dev.write_filemark();

    std::cerr << format("neotape init: initialized spool {}\n", root.string());
}

void init_tape(const Options &opts) {
    mt::TapeDevice dev(opts.target.locator, true);
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
            auto parsed = neotape::parse_fixed_header(buf.data(), buf.size());
            if (parsed.type == neotape::HeaderType::medium && !opts.force)
                fail("medium already initialized (use --force to overwrite)");
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
}

} // namespace

int neotape_init_main(int argc, char **argv) {
    try {
        auto opts = parse_args(argc, argv);
        if (opts.target.kind == "spool")
            init_spool(opts);
        else
            init_tape(opts);
        return 0;

    } catch (const std::exception &e) {
        fail(e.what());
    }
}

#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) { return neotape_init_main(argc, argv); }
#endif
