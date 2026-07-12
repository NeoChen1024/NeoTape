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

using std::format;
using std::string;

struct Options {
    string listen_address;
    string input_name = "-";
    uint32_t volume_block_size = 4 * 1024 * 1024;
    string archive_name = "raw";
    uint64_t retention_frame_count = 256;
    bool fec_enabled = false;
    std::optional<string> sign_secret_key_file;
    std::optional<string> sign_passphrase_file;
    bool debug = false;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-raw-store: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-raw-store: {}\n", msg);
    std::exit(2);
}

[[noreturn]] void fail_errno(const string &context) {
    fail(format("{}: {}", context, std::strerror(errno)));
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --listen <tcp://host:port|unix://path>\n"
        "       [--input <file|->] [--volume-block-size <bytes>]\n"
        "       [--archive-name <name>] [--retention-frame-count <N>]\n"
        "       [--fec]\n"
        "       [--sign-secret-key <file.sec>]\n"
        "       [--sign-passphrase-file <path>]\n"
        "       [--debug] [-h]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"input", required_argument, nullptr, 'i'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"archive-name", required_argument, nullptr, 'n'},
        {"retention-frame-count", required_argument, nullptr, 256},
        {"debug", no_argument, nullptr, 257},
        {"sign-secret-key", required_argument, nullptr, 258},
        {"sign-passphrase-file", required_argument, nullptr, 259},
        {"fec", no_argument, nullptr, 260},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "l:i:b:n:h", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 'l':
            opts.listen_address = optarg;
            break;
        case 'i':
            opts.input_name = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 256: {
            char *end = nullptr;
            unsigned long const n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n == 0 || n > 1000000) {
                std::cerr << "neotape-raw-store: --retention-frame-count "
                             "requires a number from 1 to 1000000\n";
                std::exit(2);
            }
            opts.retention_frame_count = static_cast<uint64_t>(n);
            break;
        }
        case 257:
            opts.debug = true;
            break;
        case 258:
            opts.sign_secret_key_file = optarg;
            break;
        case 259:
            opts.sign_passphrase_file = optarg;
            break;
        case 260:
            opts.fec_enabled = true;
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
    if (opts.sign_passphrase_file.has_value() &&
        !opts.sign_secret_key_file.has_value()) {
        usage_error("--sign-passphrase-file requires --sign-secret-key");
    }

    return opts;
}

struct FileGuard {
    FILE *file = nullptr;
    bool owned = false;
    FileGuard(FILE *f, bool own) : file(f), owned(own) {}
    ~FileGuard() {
        if (owned && file != nullptr) {
            std::fclose(file);
        }
    }
    FileGuard(const FileGuard &) = delete;
    FileGuard &operator=(const FileGuard &) = delete;
    FileGuard(FileGuard &&) = delete;
    FileGuard &operator=(FileGuard &&) = delete;
};

void produce_raw_frames(FILE *input, const string &archive_uuid,
                        const Options &opts,
                        neotape::VolumeRecordQueue &queue) {
    neotape::ContentFrameBuilder builder(opts.volume_block_size, archive_uuid,
                                         opts.archive_name, opts.fec_enabled);
    std::vector<std::byte> buf(1024ULL * 1024ULL);
    for (;;) {
        size_t const n = std::fread(buf.data(), 1, buf.size(), input);
        if (n > 0) {
            auto frames =
                builder.feed(std::span<const std::byte>(buf.data(), n));
            for (auto &frame : frames) {
                if (!queue.push(neotape::VolumeRecord{std::move(frame.record),
                                                      frame.global_seq_num,
                                                      false, false})) {
                    throw std::runtime_error("frame consumer disconnected");
                }
            }
        }
        if (n < buf.size()) {
            if (std::ferror(input) != 0) {
                throw std::runtime_error(
                    format("read input: {}", std::strerror(errno)));
            }
            break;
        }
    }

    for (auto &final_frame : builder.flush()) {
        if (!queue.push(neotape::VolumeRecord{std::move(final_frame.record),
                                              final_frame.global_seq_num, false,
                                              false})) {
            throw std::runtime_error("frame consumer disconnected");
        }
    }
    if (!queue.push(neotape::VolumeRecord{{}, 0, true, false})) {
        throw std::runtime_error("frame consumer disconnected");
    }
    if (!queue.push(neotape::VolumeRecord{
            {}, builder.next_global_seq_num(), false, true})) {
        throw std::runtime_error("frame consumer disconnected");
    }
}

uint64_t run_raw_store(FILE *input, const Options &opts) {
    neotape::VolumeServerOptions server_opts;
    server_opts.listen_address = opts.listen_address;
    server_opts.volume_block_size = opts.volume_block_size;
    server_opts.archive_name = opts.archive_name;
    server_opts.retention_frame_count = opts.retention_frame_count;
    server_opts.log_label = "raw-store";
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
    try {
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.debug;

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            fail("failed to ignore SIGPIPE");
        }

        FILE *raw_input = nullptr;
        bool owned = false;
        if (opts.input_name == "-") {
            raw_input = stdin;
        } else {
            raw_input = std::fopen(opts.input_name.c_str(), "rb");
            if (raw_input == nullptr) {
                fail_errno(string("open ") + opts.input_name);
            }
            owned = true;
        }
        FileGuard const input_guard(raw_input, owned);

        uint64_t const served = run_raw_store(raw_input, opts);
        std::cerr << format("raw-store served {} frames\n", served);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
