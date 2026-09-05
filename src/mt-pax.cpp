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
        "       [-P <buffer-percent>] [-j|--io-thread <N>]\n"
        "       [-B|--output-buffer-size <SIZE>] <path> [path ...]\n"
        "       {} -p|--plan <file> -S|--slice-output-prefix <prefix>\n"
        "       [-v|-vv] [-P <buffer-percent>] [-j|--io-thread <N>]\n"
        "       [-B|--output-buffer-size <SIZE>]\n"
        "SIZE accepts K, M, G, or T binary suffixes (for example 4M or 16G).\n",
        prog, prog);
}

[[noreturn]] void fail(const string &message) {
    neotape::write_diagnostic(format("mt-pax: {}", message));
    std::exit(1);
}

CliOptions parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"directory", required_argument, nullptr, 'C'},
        {"buffer-percent", required_argument, nullptr, 'P'},
        {"io-thread", required_argument, nullptr, 'j'},
        {"output-buffer-size", required_argument, nullptr, 'B'},
        {"plan", required_argument, nullptr, 'p'},
        {"slice-output-prefix", required_argument, nullptr, 'S'},
        {"verbose", no_argument, nullptr, 'v'},
        {"one-file-system", no_argument, nullptr, 'x'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    CliOptions opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "C:f:P:j:B:p:S:vxh", long_opts,
                            nullptr)) != -1) {
        try {
            switch (c) {
            case 'C':
                opts.writer.chdir_dir = optarg;
                break;
            case 'f':
                opts.output = optarg;
                opts.writer.output_name = optarg;
                break;
            case 'P':
                opts.writer.buffer_percent = static_cast<unsigned>(
                    neotape::parse_uint(optarg, "buffer percent", 0, 100));
                break;
            case 'v':
                opts.writer.verbose = std::min(opts.writer.verbose + 1, 2);
                break;
            case 'x':
                opts.writer.one_file_system = true;
                break;
            case 'B':
                opts.writer.output_buf_size = static_cast<size_t>(
                    neotape::parse_size(optarg, "output buffer size",
                                        std::numeric_limits<size_t>::max()));
                break;
            case 'j':
                opts.writer.io_thread = static_cast<unsigned>(
                    neotape::parse_uint(optarg, "I/O threads", 0,
                                        std::numeric_limits<unsigned>::max()));
                break;
            case 'p':
                opts.plan_path = fs::path(optarg);
                opts.writer.plan_path = opts.plan_path;
                break;
            case 'S':
                opts.slice_output_prefix = optarg;
                break;
            case 'h':
                usage(argv[0]);
                std::exit(0);
            case '?':
                std::exit(2);
            }
        } catch (const std::exception &e) {
            std::cerr << format("mt-pax: {}\n", e.what());
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

        std::cerr << format("{}  {}\n", result.write_result.blake3_hex,
                            result.output_target);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
