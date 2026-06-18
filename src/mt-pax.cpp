#include "neotape/common.hpp"
#include "neotape/pax_writer.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using std::format;
using std::string;
using std::string_view;
using std::vector;

struct CliOptions {
    neotape::PaxWriterOptions writer;
    string output;
    std::optional<fs::path> plan_path;
    std::optional<string> slice_output_prefix;
};

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} -f <out-file|-> [-v|-vv] [-x] [-C <dir>]\n"
        "       [-P <buffer-percent>] [--io-thread <N>]\n"
        "       [--output-buffer-size <bytes>] <path> [path ...]\n"
        "       {} --plan <file> --slice-output-prefix <prefix>\n"
        "       [-v|-vv] [-P <buffer-percent>] [--io-thread <N>]\n"
        "       [--output-buffer-size <bytes>]\n",
        prog, prog);
}

[[noreturn]] void fail(const string &message) {
    std::cerr << format("pax: {}\n", message);
    std::exit(1);
}

CliOptions parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"directory", required_argument, nullptr, 'C'},
        {"buffer-percent", required_argument, nullptr, 'P'},
        {"io-thread", required_argument, nullptr, 257},
        {"output-buffer-size", required_argument, nullptr, 256},
        {"plan", required_argument, nullptr, 258},
        {"slice-output-prefix", required_argument, nullptr, 259},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    CliOptions opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "C:f:P:vxh", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 'C':
            opts.writer.chdir_dir = optarg;
            break;
        case 'f':
            opts.output = optarg;
            opts.writer.output_name = optarg;
            break;
        case 'P': {
            char *end = nullptr;
            unsigned long const n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n > 100) {
                std::cerr << "pax: -P requires a percent from 0 to 100\n";
                std::exit(2);
            }
            opts.writer.buffer_percent = static_cast<unsigned>(n);
            break;
        }
        case 'v':
            opts.writer.verbose = std::min(opts.writer.verbose + 1, 2);
            break;
        case 'x':
            opts.writer.one_file_system = true;
            break;
        case 256:
            try {
                opts.writer.output_buf_size = static_cast<size_t>(
                    neotape::parse_size(optarg, "output buffer size"));
            } catch (const std::exception &e) {
                std::cerr << format("pax: {}\n", e.what());
                std::exit(2);
            }
            break;
        case 257: {
            char *end = nullptr;
            unsigned long const n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0') {
                std::cerr << "pax: --io-thread requires a number\n";
                std::exit(2);
            }
            opts.writer.io_thread = static_cast<unsigned>(n);
            break;
        }
        case 258:
            opts.plan_path = fs::path(optarg);
            opts.writer.plan_path = opts.plan_path;
            break;
        case 259:
            opts.slice_output_prefix = optarg;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    while (optind < argc) {
        opts.writer.sources.emplace_back(argv[optind++]);
    }

    if (opts.plan_path.has_value() != opts.slice_output_prefix.has_value()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (opts.slice_output_prefix.has_value() && !opts.output.empty()) {
        fail("-f cannot be used with --slice-output-prefix");
    }
    if (!opts.slice_output_prefix.has_value() && opts.output.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (!opts.plan_path.has_value() && opts.writer.sources.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (opts.plan_path.has_value() && !opts.writer.sources.empty()) {
        fail("positional sources cannot be used with --plan");
    }

    return opts;
}

} // namespace

int main(int argc, char **argv) {
    try {
        neotape::ensure_utf8_ctype_locale();
        CliOptions opts = parse_args(argc, argv);

        if (opts.slice_output_prefix.has_value()) {
            opts.slice_output_prefix =
                fs::absolute(*opts.slice_output_prefix).string();
        }
        neotape::PaxLocalOutputOptions out_opts;
        out_opts.output_path = opts.output;
        out_opts.slice_output_prefix = opts.slice_output_prefix;
        neotape::PaxLocalOutputResult result =
            neotape::write_pax_to_local_output(opts.writer, out_opts);

        std::cerr << format("\n{}  {}\n", result.write_result.blake3_hex,
                            result.output_target);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
