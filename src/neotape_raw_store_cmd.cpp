#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/frame_builder.hpp"
#include "neotape/signature.hpp"
#include "neotape/volume_server.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using neotape::fail;
using neotape::usage_error;

using std::format;
using std::string;

struct Options {
    string listen_address;
    string input_name = "-";
    uint32_t volume_block_size = 4 * 1024 * 1024;
    string archive_name = "raw";
    uint64_t retention_frame_count = 256;
    std::optional<string> sign_secret_key_file;
    std::optional<string> sign_passphrase_file;
    bool debug = false;
};

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} -l|--listen <tcp://host:port|unix://path>\n"
        "       [-i|--input <file|->] [-b|--volume-block-size <SIZE>]\n"
        "       [-n|--archive-name <name>] [-r|--retention-frame-count <N>]\n"
        "       [-k|--sign-secret-key <file.sec>]\n"
        "       [-K|--sign-passphrase-file <path>]\n"
        "       [-d|--debug] [-h]\n"
        "SIZE accepts K, M, G, or T binary suffixes (for example 4M or 16G).\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"input", required_argument, nullptr, 'i'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"archive-name", required_argument, nullptr, 'n'},
        {"retention-frame-count", required_argument, nullptr, 'r'},
        {"debug", no_argument, nullptr, 'd'},
        {"sign-secret-key", required_argument, nullptr, 'k'},
        {"sign-passphrase-file", required_argument, nullptr, 'K'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    try {
        while ((c = getopt_long(argc, argv, "l:i:b:n:r:dk:K:h", long_opts,
                                nullptr)) != -1) {
            switch (c) {
            case 'l':
                opts.listen_address = optarg;
                break;
            case 'i':
                opts.input_name = optarg;
                break;
            case 'b':
                opts.volume_block_size = static_cast<uint32_t>(
                    neotape::parse_size(optarg, "volume block size",
                                        std::numeric_limits<uint32_t>::max()));
                break;
            case 'n':
                opts.archive_name = optarg;
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
            case 'h':
                usage(argv[0]);
                std::exit(0);
            case '?':
                std::exit(2);
            default:
                usage_error(format("unexpected option code {}", c));
            }
        }

    } catch (const std::exception &e) {
        std::cerr << format("neotape-raw-store: {}\n", e.what());
        std::exit(2);
    }

    if (opts.listen_address.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (optind < argc) {
        usage_error("unexpected positional arguments");
    }
    if (!neotape::valid_block_size(opts.volume_block_size)) {
        usage_error("invalid volume block size");
    }
    if (opts.archive_name.size() >= neotape::archive_label_size) {
        usage_error(format("--archive-name is longer than {} bytes",
                           neotape::archive_label_size - 1));
    }
    if (opts.sign_passphrase_file.has_value() &&
        !opts.sign_secret_key_file.has_value()) {
        usage_error("--sign-passphrase-file requires --sign-secret-key");
    }

    return opts;
}

void produce_raw_frames(FILE *input, const string &archive_uuid,
                        const Options &opts,
                        neotape::VolumeRecordQueue &queue) {
    neotape::FrameBuilder builder(opts.volume_block_size, archive_uuid,
                                  opts.archive_name);
    std::vector<std::byte> buf(1024ULL * 1024ULL);
    for (;;) {
        size_t const n = std::fread(buf.data(), 1, buf.size(), input);
        if (n > 0) {
            neotape::push_frames(
                queue, builder.feed(std::span<const std::byte>(buf.data(), n)));
        }
        if (n < buf.size()) {
            if (std::ferror(input) != 0) {
                throw std::runtime_error(
                    format("read input: {}", std::strerror(errno)));
            }
            break;
        }
    }

    neotape::push_frames(queue, builder.flush());
    neotape::push_record(queue, neotape::VolumeRecord{.record = {},
                                                      .global_seq_num = 0,
                                                      .tape_eof = true,
                                                      .done = false});
    neotape::push_record(queue,
                         neotape::VolumeRecord{
                             .record = {},
                             .global_seq_num = builder.next_global_seq_num(),
                             .tape_eof = false,
                             .done = true,
                         });
}

neotape::VolumeServerSummary run_raw_store(FILE *input, const Options &opts) {
    neotape::VolumeServerOptions server_opts;
    server_opts.listen_address = opts.listen_address;
    server_opts.volume_block_size = opts.volume_block_size;
    server_opts.archive_name = opts.archive_name;
    server_opts.retention_frame_count = opts.retention_frame_count;
    server_opts.log_label = "neotape-raw-store";
    if (opts.sign_secret_key_file.has_value()) {
        server_opts.frame_signer = neotape::load_signify_secret_key(
            *opts.sign_secret_key_file,
            opts.sign_passphrase_file.has_value()
                ? std::optional<string>(neotape::read_signify_passphrase_file(
                      *opts.sign_passphrase_file))
                : std::nullopt);
    }

    return neotape::run_volume_server(
        server_opts,
        [&](const string &archive_uuid, neotape::VolumeRecordQueue &queue) {
            produce_raw_frames(input, archive_uuid, opts, queue);
        });
}

} // namespace

int main(int argc, char **argv) {
    neotape::program_name = "neotape-raw-store";
    try {
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.debug;

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            fail("failed to ignore SIGPIPE");
        }

        neotape::StdioFile const input =
            opts.input_name == "-"
                ? neotape::StdioFile(stdin)
                : neotape::StdioFile::open(opts.input_name, "rb");

        neotape::VolumeServerSummary const summary =
            run_raw_store(input.get(), opts);
        neotape::write_diagnostic(
            format("neotape-raw-store: archive complete: volumes={} "
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
