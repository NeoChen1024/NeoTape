#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/signature.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tape.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/writer.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using neotape::connect_to_server;
using neotape::FdGuard;
using neotape::uint64_to_le_bytes;
using std::format;
using std::string;
using std::string_view;
using std::vector;

namespace fs = std::filesystem;

constexpr uint64_t default_recovery_bundle_block_size = 256ULL * 1024;
constexpr int exit_volume_change_required = 3;

using TargetLocator = neotape::MediaLocator;

struct Options {
    string source_address;
    TargetLocator target;
    bool erase = false;
    bool append = false;
    std::optional<fs::path> recovery_bundle;
    std::optional<uint64_t> recovery_bundle_block_size;
    size_t output_buffer_size = 256ULL * 1024 * 1024;
    std::optional<uint64_t> max_volume_bytes;
    vector<string> verify_pubkey_paths;
    bool debug = false;
};

string_view target_kind_name(TargetLocator::Kind kind) {
    switch (kind) {
    case TargetLocator::tape:
        return "tape";
    case TargetLocator::spool:
        return "spool";
    case TargetLocator::null_sink:
        return "null";
    case TargetLocator::none:
        break;
    }
    return "unknown";
}

void report_volume_capacity_reached(TargetLocator::Kind kind,
                                    uint64_t committed_frames) {
    if (kind == TargetLocator::tape) {
        neotape::write_diagnostic(
            format("neotape-write: end of tape reached: committed_frames={}",
                   committed_frames));
        return;
    }
    neotape::write_diagnostic(
        format("neotape-write: volume capacity reached: target={} "
               "committed_frames={}",
               target_kind_name(kind), committed_frames));
}

[[noreturn]] void fail(const string &msg) {
    neotape::write_diagnostic(format("neotape-write: {}", msg));
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(2);
}

void usage(const char *prog) {
    std::cerr << format("usage: {} -s|--source <tcp://host:port|unix://path>\n"
                        "       -t|--target <tape:/dev/nst0|spool:./dir|null>\n"
                        "       [-k|--verify-pubkey <file.pub>]...\n"
                        "       [-e|--erase | -a|--append]\n"
                        "       [-R|--recovery-bundle <tar>]\n"
                        "       [-r|--recovery-bundle-block-size <SIZE>] "
                        "(default 256K)\n"
                        "       [-B|--output-buffer-size <SIZE>]\n"
                        "       [-m|--max-volume-bytes <SIZE>] [-d|--debug]\n"
                        "SIZE accepts K, M, G, or T binary suffixes "
                        "(for example 4M or 16G).\n",
                        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"target", required_argument, nullptr, 't'},
        {"erase", no_argument, nullptr, 'e'},
        {"append", no_argument, nullptr, 'a'},
        {"recovery-bundle", required_argument, nullptr, 'R'},
        {"recovery-bundle-block-size", required_argument, nullptr, 'r'},
        {"output-buffer-size", required_argument, nullptr, 'B'},
        {"max-volume-bytes", required_argument, nullptr, 'm'},
        {"debug", no_argument, nullptr, 'd'},
        {"verify-pubkey", required_argument, nullptr, 'k'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "s:t:eaR:r:B:m:dk:h", long_opts,
                            nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source_address = optarg;
            break;
        case 't':
            opts.target = neotape::parse_media(optarg, true);
            break;
        case 'e':
            opts.erase = true;
            break;
        case 'a':
            opts.append = true;
            break;
        case 'R':
            opts.recovery_bundle = fs::path(optarg);
            break;
        case 'r': {
            try {
                opts.recovery_bundle_block_size =
                    neotape::parse_size(optarg, "recovery bundle block size");
            } catch (const std::exception &e) {
                std::cerr << format("neotape-write: {}\n", e.what());
                std::exit(2);
            }
            break;
        }
        case 'B': {
            try {
                opts.output_buffer_size = static_cast<size_t>(
                    neotape::parse_size(optarg, "output buffer size",
                                        std::numeric_limits<size_t>::max()));
            } catch (const std::exception &e) {
                std::cerr << format("neotape-write: {}\n", e.what());
                std::exit(2);
            }
            break;
        }
        case 'm': {
            try {
                opts.max_volume_bytes =
                    neotape::parse_size(optarg, "max volume bytes");
            } catch (const std::exception &e) {
                std::cerr << format("neotape-write: {}\n", e.what());
                std::exit(2);
            }
            break;
        }
        case 'd':
            opts.debug = true;
            break;
        case 'k':
            opts.verify_pubkey_paths.emplace_back(optarg);
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.source_address.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (opts.target.kind == TargetLocator::none) {
        usage_error("--target is required");
    }
    if (opts.erase && opts.append) {
        usage_error("--erase and --append are mutually exclusive");
    }
    if (opts.append && opts.recovery_bundle.has_value()) {
        usage_error("--recovery-bundle cannot be used with --append");
    }
    if (opts.target.kind == TargetLocator::null_sink &&
        (opts.erase || opts.append)) {
        usage_error("--erase and --append are not valid with a null target");
    }
    if (opts.target.kind == TargetLocator::null_sink &&
        opts.recovery_bundle.has_value()) {
        usage_error("--recovery-bundle is not valid with a null target");
    }
    if (opts.recovery_bundle_block_size.has_value() &&
        !opts.recovery_bundle.has_value()) {
        usage_error("--recovery-bundle-block-size requires "
                    "--recovery-bundle");
    }
    if (opts.recovery_bundle_block_size.has_value() &&
        opts.target.kind != TargetLocator::tape) {
        usage_error(
            "--recovery-bundle-block-size is only valid with tape targets");
    }
    if (opts.recovery_bundle_block_size.has_value() &&
        (*opts.recovery_bundle_block_size > neotape::max_block_size ||
         *opts.recovery_bundle_block_size < neotape::min_block_size ||
         *opts.recovery_bundle_block_size % 1024 != 0)) {
        usage_error(format(
            "--recovery-bundle-block-size must be from {} to {} bytes and a "
            "multiple of 1 KiB",
            neotape::min_block_size, neotape::max_block_size));
    }

    if (opts.recovery_bundle.has_value()) {
        std::error_code ec;
        if (!fs::is_regular_file(*opts.recovery_bundle, ec) || ec) {
            usage_error(format("recovery bundle is not a regular file: {}",
                               opts.recovery_bundle->string()));
        }
        uintmax_t const size = fs::file_size(*opts.recovery_bundle, ec);
        if (ec) {
            usage_error(format("cannot stat recovery bundle {}: {}",
                               opts.recovery_bundle->string(), ec.message()));
        }
        if (size == 0) {
            usage_error("recovery bundle must not be empty");
        }
    }

    if (opts.max_volume_bytes.has_value() &&
        opts.target.kind != TargetLocator::spool &&
        opts.target.kind != TargetLocator::null_sink) {
        usage_error(
            "--max-volume-bytes is only valid with spool or null targets");
    }

    constexpr size_t min_output_buffer_size = 8ULL * 1024 * 1024;
    if (opts.output_buffer_size < min_output_buffer_size) {
        usage_error("--output-buffer-size must be at least 8 MiB");
    }

    return opts;
}

string decode_error_payload(const vector<std::byte> &payload,
                            string_view fallback) {
    string reason;
    reason.reserve(payload.size());
    for (std::byte const b : payload) {
        reason.push_back(static_cast<char>(b));
    }
    if (reason.empty()) {
        reason = fallback;
    }
    return reason;
}

std::vector<std::byte>
auth_challenge_payload(const neotape::AuthNonceBytes &nonce) {
    std::vector<std::byte> payload(nonce.size());
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        payload[i] = static_cast<std::byte>(nonce[i]);
    }
    return payload;
}

neotape::DetachedSignatureBytes
decode_auth_response_payload(const vector<std::byte> &payload) {
    if (payload.size() != neotape::DetachedSignatureBytes{}.size()) {
        throw std::runtime_error(
            format("auth_response payload must be {} bytes",
                   neotape::DetachedSignatureBytes{}.size()));
    }

    neotape::DetachedSignatureBytes signature{};
    for (std::size_t i = 0; i < signature.size(); ++i) {
        signature[i] = static_cast<uint8_t>(payload[i]);
    }
    return signature;
}

void authenticate_source_server(int fd,
                                const vector<neotape::SignifyPublicKey> &keys) {
    if (keys.empty()) {
        return;
    }

    neotape::AuthNonceBytes const nonce = neotape::random_auth_nonce();
    neotape::tcp::write_message(
        fd, neotape::tcp::Message{neotape::tcp::MessageType::auth_challenge,
                                  auth_challenge_payload(nonce)});
    auto response = neotape::tcp::read_message(fd);
    if (!response.has_value()) {
        throw std::runtime_error("unexpected disconnect during auth handshake");
    }

    using neotape::tcp::MessageType;
    switch (response->type) {
    case MessageType::auth_response: {
        neotape::DetachedSignatureBytes const signature =
            decode_auth_response_payload(response->payload);
        bool verified = false;
        for (const auto &key : keys) {
            if (neotape::verify_auth_nonce_signature(signature, nonce, key)) {
                verified = true;
                break;
            }
        }
        if (!verified) {
            throw std::runtime_error(
                "auth_response signature verification failed");
        }
        return;
    }
    case MessageType::error:
        throw std::runtime_error(
            decode_error_payload(response->payload, "archiver auth failure"));
    default:
        throw std::runtime_error(
            format("expected auth_response, got {}",
                   neotape::tcp::message_type_name(response->type)));
    }
}

uint64_t install_spool_recovery_bundle(const fs::path &root,
                                       const fs::path &source) {
    fs::create_directories(root);
    fs::path const destination = root / "recovery-bundle.tar";
    fs::path const pending =
        root / format("recovery-bundle.tar.pending.{}", ::getpid());
    std::error_code ec;
    fs::remove(pending, ec);
    ec.clear();
    fs::copy_file(source, pending, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error(
            format("copy recovery bundle: {}", ec.message()));
    }
    uint64_t const size = fs::file_size(pending, ec);
    if (ec) {
        fs::remove(pending);
        throw std::runtime_error(
            format("stat copied recovery bundle: {}", ec.message()));
    }
    fs::rename(pending, destination, ec);
    if (ec) {
        fs::remove(pending);
        throw std::runtime_error(
            format("install recovery bundle: {}", ec.message()));
    }
    return size;
}

void write_tape_recovery_bundle(mt::TapeDevice &dev, const fs::path &source,
                                size_t record_size) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            format("cannot open recovery bundle: {}", source.string()));
    }

    vector<std::byte> record(record_size);
    uint64_t source_bytes = 0;
    uint64_t record_count = 0;
    for (;;) {
        std::fill(record.begin(), record.end(), std::byte{0});
        input.read(reinterpret_cast<char *>(record.data()),
                   static_cast<std::streamsize>(record.size()));
        std::streamsize const n = input.gcount();
        if (n == 0) {
            if (!input.eof()) {
                throw std::runtime_error("error reading recovery bundle");
            }
            break;
        }
        source_bytes += static_cast<uint64_t>(n);
        dev.write_record(record.data(), record.size());
        ++record_count;
        if (n < static_cast<std::streamsize>(record.size())) {
            if (!input.eof()) {
                throw std::runtime_error("error reading recovery bundle");
            }
            break;
        }
    }
    dev.write_filemark();
    std::cerr << format("neotape-write: wrote recovery bundle {} ({} source "
                        "bytes, {} tape records "
                        "of {} bytes)\n",
                        source.string(), source_bytes, record_count,
                        record_size);
}

} // namespace

int main(int argc, char **argv) {
    // Report socket failures through the session rather than SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);

    try {
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.debug;
        vector<neotape::SignifyPublicKey> verify_keys;
        verify_keys.reserve(opts.verify_pubkey_paths.size());
        for (const string &path : opts.verify_pubkey_paths) {
            verify_keys.push_back(neotape::load_signify_public_key(path));
        }

        FdGuard const fd_guard(connect_to_server(opts.source_address));
        int fd = fd_guard.fd;
        authenticate_source_server(fd, verify_keys);

        std::unique_ptr<mt::TapeDevice> output;
        uint64_t recovery_bundle_bytes = 0;

        {
            std::unique_ptr<mt::TapeDevice> dev;
            if (opts.target.kind == TargetLocator::tape) {
                dev = std::make_unique<mt::TapeDevice>(opts.target.path, true);
            } else if (opts.target.kind == TargetLocator::spool) {
                fs::path const spool_root(opts.target.path);
                fs::path const installed_bundle =
                    spool_root / "recovery-bundle.tar";
                if (!opts.erase && !opts.append &&
                    fs::exists(installed_bundle)) {
                    fail("spool appears to contain a recovery bundle; use "
                         "--erase or --append");
                }
                dev = std::make_unique<mt::SpoolTapeDevice>(
                    std::filesystem::path(opts.target.path), true);
            }

            // Default policy: refuse to overwrite existing tape content.
            if (dev && !opts.erase && !opts.append) {
                auto st = dev->status();
                if (!st.bot()) {
                    fail("tape is not at BOT; use --erase or --append");
                }
                // BOT alone doesn't guarantee the tape is empty.  If the
                // drive reports EOD at BOT, the tape is empty; otherwise
                // there is at least one record after BOT.
                if (!st.eod()) {
                    fail("tape appears to contain data; use --erase or "
                         "--append");
                }
            }

            if (opts.append) {
                dev->space_to_eod();
            } else if (dev) {
                dev->rewind(); // --erase or empty tape
            }

            if (opts.target.kind == TargetLocator::tape &&
                opts.recovery_bundle.has_value()) {
                uint64_t const bundle_block_size =
                    opts.recovery_bundle_block_size.value_or(
                        default_recovery_bundle_block_size);
                write_tape_recovery_bundle(
                    *dev, *opts.recovery_bundle,
                    static_cast<size_t>(bundle_block_size));
            } else if (opts.target.kind == TargetLocator::spool) {
                fs::path const spool_root(opts.target.path);
                fs::path const installed_bundle =
                    spool_root / "recovery-bundle.tar";
                if (opts.recovery_bundle.has_value()) {
                    recovery_bundle_bytes = install_spool_recovery_bundle(
                        spool_root, *opts.recovery_bundle);
                    std::cerr << format("neotape-write: installed recovery "
                                        "bundle {} ({} bytes)\n",
                                        installed_bundle.string(),
                                        recovery_bundle_bytes);
                } else if (opts.erase) {
                    std::error_code ec;
                    fs::remove(installed_bundle, ec);
                    if (ec) {
                        fail(format("remove stale recovery bundle: {}",
                                    ec.message()));
                    }
                }
            }

            output = std::move(dev);
        }

        neotape::RecordSink sink(output.get(), opts.max_volume_bytes,
                                 recovery_bundle_bytes);
        auto result = neotape::write_volume(fd, sink, verify_keys,
                                            opts.output_buffer_size);
        if (result.status == neotape::WriteStatus::volume_full) {
            report_volume_capacity_reached(opts.target.kind, result.frames);
            return exit_volume_change_required;
        }
        neotape::write_diagnostic(
            format("neotape-write: archive complete: target={} "
                   "volume_frames={} final_global_seq={}",
                   target_kind_name(opts.target.kind), result.frames,
                   result.final_global_seq));
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
