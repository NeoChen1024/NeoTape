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
    bool fec_enabled = false;
    neotape::PaxWriterOptions pax;
    std::optional<string> sign_secret_key_file;
    std::optional<string> sign_passphrase_file;
    bool debug = false;
};

[[noreturn]] void fail(const string &msg) {
    neotape::write_diagnostic(format("neotape-archiver: {}", msg));
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage:\n"
        "  {} -l|--listen <tcp://host:port|unix://path>\n"
        "       [-b|--volume-block-size <SIZE>] [-n|--archive-name <name>]\n"
        "       [-C <dir>] [-P <percent>] [-j|--io-thread <N>]\n"
        "       [-B|--output-buffer-size <SIZE>] [-p|--plan <file>]\n"
        "       [-r|--retention-frame-count <N>] [-F|--fec]\n"
        "       [-k|--sign-secret-key <file.sec>]\n"
        "       [-K|--sign-passphrase-file <path>] [-v|-vv] [-x] [-d|--debug]\n"
        "       <path> [path...]\n"
        "SIZE accepts K, M, G, or T binary suffixes (for example 4M or 16G).\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"archive-name", required_argument, nullptr, 'n'},
        {"directory", required_argument, nullptr, 'C'},
        {"buffer-percent", required_argument, nullptr, 'P'},
        {"io-thread", required_argument, nullptr, 'j'},
        {"output-buffer-size", required_argument, nullptr, 'B'},
        {"plan", required_argument, nullptr, 'p'},
        {"retention-frame-count", required_argument, nullptr, 'r'},
        {"verbose", no_argument, nullptr, 'v'},
        {"one-file-system", no_argument, nullptr, 'x'},
        {"debug", no_argument, nullptr, 'd'},
        {"sign-secret-key", required_argument, nullptr, 'k'},
        {"sign-passphrase-file", required_argument, nullptr, 'K'},
        {"fec", no_argument, nullptr, 'F'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "l:b:n:C:P:j:B:p:r:dk:K:Fvxh",
                            long_opts, nullptr)) != -1) {
        try {
            switch (c) {
            case 'l':
                opts.listen_address = optarg;
                break;
            case 'b':
                opts.volume_block_size = static_cast<uint32_t>(
                    neotape::parse_size(optarg, "volume block size",
                                        std::numeric_limits<uint32_t>::max()));
                break;
            case 'n':
                opts.archive_name = optarg;
                break;
            case 'C':
                opts.pax.chdir_dir = optarg;
                break;
            case 'P':
                opts.pax.buffer_percent = static_cast<unsigned>(
                    neotape::parse_uint(optarg, "buffer percent", 0, 100));
                break;
            case 'v':
                opts.pax.verbose = std::min(opts.pax.verbose + 1, 2);
                break;
            case 'x':
                opts.pax.one_file_system = true;
                break;
            case 'B':
                opts.pax.output_buf_size = static_cast<size_t>(
                    neotape::parse_size(optarg, "output buffer size",
                                        std::numeric_limits<size_t>::max()));
                break;
            case 'j':
                opts.pax.io_thread = static_cast<unsigned>(
                    neotape::parse_uint(optarg, "I/O threads", 0,
                                        std::numeric_limits<unsigned>::max()));
                break;
            case 'p':
                opts.pax.plan_path = optarg;
                break;
            case 'r':
                opts.retention_frame_count =
                    static_cast<uint64_t>(neotape::parse_uint(
                        optarg, "retention frame count", 1, 1000000));
                break;
            case 'd':
                opts.debug = true;
                break;
            case 'k':
                opts.sign_secret_key_file = optarg;
                break;
            case 'K':
                opts.sign_passphrase_file = optarg;
                break;
            case 'F':
                opts.fec_enabled = true;
                break;
            case 'h':
                usage(argv[0]);
                std::exit(0);
            case '?':
                std::exit(2);
            }
        } catch (const std::exception &e) {
            std::cerr << format("neotape-archiver: {}\n", e.what());
            std::exit(2);
        }
    }

    while (optind < argc) {
        opts.pax.sources.emplace_back(argv[optind++]);
    }

    if (opts.listen_address.empty()) {
        fail("--listen is required");
    }
    if (opts.sign_passphrase_file.has_value() &&
        !opts.sign_secret_key_file.has_value()) {
        fail("--sign-passphrase-file requires --sign-secret-key");
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

        neotape::TcpArchiverOptions server_opts;
        server_opts.listen_address = opts.listen_address;
        server_opts.volume_block_size = opts.volume_block_size;
        server_opts.archive_name = opts.archive_name;
        server_opts.retention_frame_count = opts.retention_frame_count;
        server_opts.fec_enabled = opts.fec_enabled;
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

        neotape::VolumeServerSummary const summary =
            neotape::run_tcp_archiver(server_opts);
        neotape::write_diagnostic(
            format("neotape-archiver: archive complete: volumes={} "
                   "committed_frames={} frame_transmissions={} connections={} "
                   "uncommitted_disconnects={}",
                   summary.committed_volumes, summary.committed_frames,
                   summary.frame_transmissions, summary.connections,
                   summary.uncommitted_disconnects));
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
