#include "neotape/common.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/resume.hpp"
#include "neotape/signature.hpp"
#include "neotape/tcp_server.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using neotape::fail;
using neotape::usage_error;

using std::format;
using std::string;

struct Options {
    string listen_address;
    std::optional<uint32_t> volume_block_size;
    std::optional<string> archive_name;
    uint64_t retention_frame_count = 256;
    neotape::PaxWriterOptions pax;
    std::optional<bool> write_plan_catalog;
    std::optional<string> resume_file;
    // A slice number or "last".
    std::optional<string> resume_from_slice;
    std::optional<string> sign_secret_key_file;
    std::optional<string> sign_passphrase_file;
    bool debug = false;
};

bool parse_plan_write_mode(std::string_view text) {
    if (text == "no")
        return false;
    if (text == "slice0")
        return true;
    throw std::runtime_error(
        format("invalid plan write mode '{}': expected no or slice0", text));
}

void usage(const char *prog) {
    std::cerr << format(
        "usage:\n"
        "  {} -l|--listen <tcp://host:port|unix://path>\n"
        "       [-b|--volume-block-size <SIZE>] [-n|--archive-name <name>]\n"
        "       [-C <dir>] [-P <percent>] [-j|--io-thread <N>]\n"
        "       [-B|--output-buffer-size <SIZE>] [-p|--plan <file>]\n"
        "       [-w|--plan-write-mode no|slice0]\n"
        "       [-f|--resume-file <file> [-s|--resume-from-slice <N|last>]]\n"
        "       [-r|--retention-frame-count <N>]\n"
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
        {"plan-write-mode", required_argument, nullptr, 'w'},
        {"resume-file", required_argument, nullptr, 'f'},
        {"resume-from-slice", required_argument, nullptr, 's'},
        {"one-file-system", no_argument, nullptr, 'x'},
        {"debug", no_argument, nullptr, 'd'},
        {"sign-secret-key", required_argument, nullptr, 'k'},
        {"sign-passphrase-file", required_argument, nullptr, 'K'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "l:b:n:C:P:j:B:p:w:f:s:r:dk:K:vxh",
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
            case 'w':
                opts.write_plan_catalog = parse_plan_write_mode(optarg);
                break;
            case 'f':
                opts.resume_file = optarg;
                break;
            case 's':
                opts.resume_from_slice = optarg;
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
    if (!has_plan && opts.write_plan_catalog.has_value()) {
        fail("--plan-write-mode requires --plan");
    }
    if (!has_plan && opts.resume_file.has_value()) {
        fail("--resume-file requires --plan");
    }
    if (opts.resume_from_slice.has_value() && !opts.resume_file.has_value()) {
        fail("--resume-from-slice requires --resume-file");
    }
    if (!has_plan && !has_sources) {
        usage(argv[0]);
        std::exit(2);
    }

    return opts;
}

// Apply --resume-file: start a new resume file, or continue the archive it
// describes from the requested slice.
std::unique_ptr<neotape::ResumeLog>
setup_resume(const Options &opts, neotape::TcpArchiverOptions &server_opts) {
    using neotape::ResumeHeader;
    const string &path = *opts.resume_file;
    string const key_id =
        server_opts.frame_signer
            ? neotape::hex_encode(server_opts.frame_signer->key_id)
            : string{};
    string const plan_hash = neotape::blake3_file_hex(*opts.pax.plan_path);

    if (!opts.resume_from_slice) {
        server_opts.archive_uuid = neotape::make_uuid_v4();
        return std::make_unique<neotape::ResumeLog>(
            path, ResumeHeader{
                      .archive_uuid = *server_opts.archive_uuid,
                      .block_size_kib = server_opts.volume_block_size / 1024,
                      .sign_key_id = key_id,
                      .plan_blake3 = plan_hash,
                      .archive_name = server_opts.archive_name,
                  });
    }

    neotape::ResumeState const state = neotape::read_resume_file(path);
    const ResumeHeader &header = state.header;
    if (state.points.empty())
        throw std::runtime_error(
            format("resume file {} records no slice start; nothing was "
                   "completed, so start the archive again with a new file",
                   path));
    neotape::ResumePoint point = state.points.back();
    if (*opts.resume_from_slice != "last") {
        uint64_t const slice =
            neotape::parse_uint(*opts.resume_from_slice, "resume slice", 1);
        auto it = std::ranges::find(state.points, slice,
                                    &neotape::ResumePoint::slice);
        if (it == state.points.end())
            throw std::runtime_error(
                format("resume file {} has no start recorded for slice {}; the "
                       "recorded slices are {}..{}",
                       path, slice, state.points.front().slice,
                       state.points.back().slice));
        point = *it;
    }

    if (opts.volume_block_size &&
        *opts.volume_block_size / 1024 != header.block_size_kib)
        throw std::runtime_error(
            format("--volume-block-size differs from the resumed archive "
                   "({} KiB)",
                   header.block_size_kib));
    if (opts.archive_name && *opts.archive_name != header.archive_name)
        throw std::runtime_error(
            format("--archive-name differs from the resumed archive (\"{}\")",
                   neotape::escape_bytes_for_diagnostic(header.archive_name)));
    if (key_id != header.sign_key_id)
        throw std::runtime_error(format(
            "signing key differs from the resumed archive (was {})",
            header.sign_key_id.empty() ? "unsigned" : header.sign_key_id));
    if (header.plan_blake3.empty()) {
        neotape::write_diagnostic(
            format("neotape-archiver: warning: resume file {} has no plan "
                   "hash; cannot check that {} is the original plan",
                   path, opts.pax.plan_path->string()));
    } else if (header.plan_blake3 != plan_hash) {
        throw std::runtime_error(
            format("plan {} is not the plan the archive was started with",
                   opts.pax.plan_path->string()));
    }

    server_opts.archive_uuid = header.archive_uuid;
    server_opts.archive_name = header.archive_name;
    server_opts.volume_block_size = header.block_size_kib * 1024;
    server_opts.first_global_seq = point.global_seq;
    server_opts.initial_volume_seq_num = point.volume;
    server_opts.pax.first_slice = point.slice;
    neotape::write_diagnostic(format(
        "neotape-archiver: resuming archive {} from slice {} at "
        "global_seq={} on volume {}",
        header.archive_uuid, point.slice, point.global_seq, point.volume));
    return std::make_unique<neotape::ResumeLog>(path, state, point);
}

} // namespace

int main(int argc, char **argv) {
    neotape::program_name = "neotape-archiver";
    try {
        neotape::ensure_utf8_ctype_locale();
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.debug;

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            fail("failed to ignore SIGPIPE");
        }

        neotape::TcpArchiverOptions server_opts;
        server_opts.listen_address = opts.listen_address;
        if (opts.volume_block_size)
            server_opts.volume_block_size = *opts.volume_block_size;
        server_opts.archive_name = opts.archive_name.value_or("archive");
        server_opts.retention_frame_count = opts.retention_frame_count;
        server_opts.pax = opts.pax;
        if (opts.write_plan_catalog)
            server_opts.write_plan_catalog = *opts.write_plan_catalog;
        if (opts.sign_secret_key_file.has_value()) {
            server_opts.frame_signer = neotape::load_signify_secret_key(
                *opts.sign_secret_key_file,
                opts.sign_passphrase_file.has_value()
                    ? std::optional<string>(
                          neotape::read_signify_passphrase_file(
                              *opts.sign_passphrase_file))
                    : std::nullopt);
        }

        std::unique_ptr<neotape::ResumeLog> resume_log;
        if (opts.resume_file) {
            resume_log = setup_resume(opts, server_opts);
            server_opts.resume_log = resume_log.get();
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
