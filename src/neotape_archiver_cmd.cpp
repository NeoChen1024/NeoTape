#include "neotape/common.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/signature.hpp"
#include "neotape/tcp_server.hpp"

#include <csignal>
#include <cstdlib>
#include <format>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using std::format;
using std::string;

struct Options {
    string listen_address;
    uint32_t volume_block_size = 4 * 1024 * 1024;
    string archive_name = "archive";
    uint64_t retention_frame_count = 256;
    neotape::PaxWriterOptions pax;
    bool explicit_output = false;
    std::optional<string> sign_secret_key_file;
    std::optional<string> sign_passphrase_file;
    bool debug = false;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-archiver: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage (server mode):\n"
        "  {} --listen <tcp://host:port|unix://path>\n"
        "       [--volume-block-size <bytes>] [--archive-name <name>]\n"
        "       [-C <dir>] [-P <percent>] [--io-thread <N>]\n"
        "       [--output-buffer-size <bytes>] [--plan <file>]\n"
        "       [--retention-frame-count <N>] [--sign-secret-key <file.sec>]\n"
        "       [--sign-passphrase-file <path>] [-v|-vv] [-x] [--debug]\n"
        "       <path> [path...]\n"
        "usage (local mode):\n"
        "  {} -f <out-file|-> [-C <dir>] [-P <percent>] [--io-thread <N>]\n"
        "       [--output-buffer-size <bytes>] [--plan <file>] [-v|-vv] [-x]\n"
        "       <path> [path...]\n",
        prog, prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"archive-name", required_argument, nullptr, 'n'},
        {"directory", required_argument, nullptr, 'C'},
        {"buffer-percent", required_argument, nullptr, 'P'},
        {"io-thread", required_argument, nullptr, 257},
        {"output-buffer-size", required_argument, nullptr, 256},
        {"plan", required_argument, nullptr, 258},
        {"retention-frame-count", required_argument, nullptr, 259},
        {"verbose", no_argument, nullptr, 'v'},
        {"one-file-system", no_argument, nullptr, 'x'},
        {"debug", no_argument, nullptr, 260},
        {"sign-secret-key", required_argument, nullptr, 261},
        {"sign-passphrase-file", required_argument, nullptr, 262},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "l:b:n:f:C:P:vxh", long_opts,
                            nullptr)) != -1) {
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
        case 'f':
            opts.pax.output_name = optarg;
            opts.explicit_output = true;
            break;
        case 'C':
            opts.pax.chdir_dir = optarg;
            break;
        case 'P': {
            char *end = nullptr;
            unsigned long const n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n > 100) {
                std::cerr << "neotape-archiver: -P requires a percent from 0 "
                             "to 100\n";
                std::exit(2);
            }
            opts.pax.buffer_percent = static_cast<unsigned>(n);
            break;
        }
        case 'v':
            opts.pax.verbose = std::min(opts.pax.verbose + 1, 2);
            break;
        case 'x':
            opts.pax.one_file_system = true;
            break;
        case 256:
            try {
                opts.pax.output_buf_size = static_cast<size_t>(
                    neotape::parse_size(optarg, "output buffer size"));
            } catch (const std::exception &e) {
                std::cerr << format("neotape-archiver: {}\n", e.what());
                std::exit(2);
            }
            break;
        case 257: {
            char *end = nullptr;
            unsigned long const n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0') {
                std::cerr
                    << "neotape-archiver: --io-thread requires a number\n";
                std::exit(2);
            }
            opts.pax.io_thread = static_cast<unsigned>(n);
            break;
        }
        case 258:
            opts.pax.plan_path = optarg;
            break;
        case 259: {
            char *end = nullptr;
            unsigned long const n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n == 0 || n > 1000000) {
                std::cerr << "neotape-archiver: --retention-frame-count "
                             "requires a number from 1 to 1000000\n";
                std::exit(2);
            }
            opts.retention_frame_count = static_cast<uint64_t>(n);
            break;
        }
        case 260:
            opts.debug = true;
            break;
        case 261:
            opts.sign_secret_key_file = optarg;
            break;
        case 262:
            opts.sign_passphrase_file = optarg;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    while (optind < argc) {
        opts.pax.sources.emplace_back(argv[optind++]);
    }

    if (!opts.listen_address.empty() && opts.explicit_output) {
        fail("-f cannot be used with --listen");
    }
    if (opts.sign_passphrase_file.has_value() &&
        !opts.sign_secret_key_file.has_value()) {
        fail("--sign-passphrase-file requires --sign-secret-key");
    }
    if (opts.sign_secret_key_file.has_value() && opts.listen_address.empty()) {
        fail("--sign-secret-key is only valid with --listen");
    }
    bool const has_plan = opts.pax.plan_path.has_value();
    bool const has_sources = !opts.pax.sources.empty();
    if (has_plan && has_sources) {
        fail("positional sources cannot be used with --plan");
    }
    if (!has_plan && !has_sources) {
        usage(argv[0]);
        std::exit(2);
    }

    return opts;
}

} // namespace

int main(int argc, char **argv) {
    try {
        neotape::ensure_utf8_ctype_locale();
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.debug;

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            fail("failed to ignore SIGPIPE");
        }

        if (!opts.listen_address.empty()) {
            neotape::TcpArchiverOptions server_opts;
            server_opts.listen_address = opts.listen_address;
            server_opts.volume_block_size = opts.volume_block_size;
            server_opts.archive_name = opts.archive_name;
            server_opts.retention_frame_count = opts.retention_frame_count;
            server_opts.pax = opts.pax;
            if (opts.sign_secret_key_file.has_value()) {
                server_opts.frame_signer = neotape::load_signify_secret_key(
                    *opts.sign_secret_key_file,
                    opts.sign_passphrase_file.has_value()
                        ? std::optional<string>(
                              neotape::read_signify_passphrase_file(
                                  *opts.sign_passphrase_file))
                        : std::nullopt);
            }

            uint64_t served = neotape::run_tcp_archiver(server_opts);
            std::cerr << format("archiver served {} frames\n", served);
            return 0;
        }

        neotape::PaxLocalOutputOptions out_opts;
        out_opts.output_path = opts.pax.output_name;
        neotape::PaxLocalOutputResult result =
            neotape::write_pax_to_local_output(opts.pax, out_opts);

        std::cerr << format("{}  {}\n", result.write_result.blake3_hex,
                            result.output_target);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
