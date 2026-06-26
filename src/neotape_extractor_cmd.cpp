#include "neotape/common.hpp"
#include "neotape/extractor.hpp"
#include "neotape/signature.hpp"

#include <csignal>
#include <cstdlib>
#include <format>
#include <getopt.h>
#include <iostream>
#include <string>
#include <vector>

namespace {

using std::format;
using std::string;

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-extractor: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format("usage: {} --listen <tcp://host:port|unix://path>\n"
                        "       [-o <file>] [--verify-pubkey <file.pub>]...\n"
                        "       [--require-signed] [-v] [-h]\n",
                        prog);
}

struct Options {
    string listen_address;
    string output_path;
    bool verbose = false;
    bool require_signed = false;
    std::vector<string> verify_pubkey_paths;
};

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"output", required_argument, nullptr, 'o'},
        {"verbose", no_argument, nullptr, 'v'},
        {"verify-pubkey", required_argument, nullptr, 256},
        {"require-signed", no_argument, nullptr, 257},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "l:o:vh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'l':
            opts.listen_address = optarg;
            break;
        case 'o':
            opts.output_path = optarg;
            break;
        case 'v':
            opts.verbose = true;
            break;
        case 256:
            opts.verify_pubkey_paths.emplace_back(optarg);
            break;
        case 257:
            opts.require_signed = true;
            break;
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
    if (optind < argc) {
        std::cerr << format(
            "neotape-extractor: unexpected positional arguments\n");
        std::exit(2);
    }

    return opts;
}

} // namespace

int main(int argc, char **argv) {
    try {
        neotape::ensure_utf8_ctype_locale();
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.verbose;

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            fail("failed to ignore SIGPIPE");
        }

        neotape::ExtractorOptions ex_opts;
        ex_opts.listen_address = opts.listen_address;
        ex_opts.output_path = opts.output_path;
        ex_opts.verbose = opts.verbose;
        ex_opts.require_signed = opts.require_signed;
        for (const string &path : opts.verify_pubkey_paths) {
            ex_opts.verify_keys.push_back(neotape::load_signify_public_key(path));
        }

        uint64_t const frames = neotape::run_tcp_extractor(ex_opts);
        std::cerr << format("extractor validated {} frames\n", frames);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
