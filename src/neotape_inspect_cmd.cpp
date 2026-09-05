#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/signature.hpp"
#include "neotape/tape.hpp"
#include "neotape/validate.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

using neotape::ChannelType;
using neotape::FrameHeader;
using neotape::FrameValidator;
using neotape::has_frame_flag_clean_end;
using neotape::has_frame_flag_end;
using neotape::has_frame_flag_signed;
using neotape::Hash;
using std::format;
using std::string;
using std::string_view;
using std::vector;

namespace fs = std::filesystem;

using SourceLocator = neotape::MediaLocator;
using neotape::parse_media;

struct Options {
    SourceLocator source;
    bool debug = false;
    bool raw = false; // raw header dump
    bool require_signed = false;
    vector<string> verify_pubkey_paths;
    vector<neotape::SignifyPublicKey> verify_keys;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-inspect: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-inspect: {}\n", msg);
    std::exit(2);
}

void usage(const char *prog) {
    std::cerr << format("usage: {} -s|--source <spool:./dir|tape:/dev/nst0>\n"
                        "       [-k|--verify-pubkey <file.pub>]...\n"
                        "       [-S|--require-signed] [-d|--debug] [-r|--raw] "
                        "[-h]\n",
                        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"debug", no_argument, nullptr, 'd'},
        {"raw", no_argument, nullptr, 'r'},
        {"verify-pubkey", required_argument, nullptr, 'k'},
        {"require-signed", no_argument, nullptr, 'S'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "s:drk:Sh", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 's':
            opts.source = parse_media(optarg);
            break;
        case 'd':
            opts.debug = true;
            break;
        case 'r':
            opts.raw = true;
            break;
        case 'k':
            opts.verify_pubkey_paths.emplace_back(optarg);
            break;
        case 'S':
            opts.require_signed = true;
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

    if (opts.source.kind == SourceLocator::none) {
        usage_error("--source is required");
    }
    if (opts.require_signed && opts.verify_pubkey_paths.empty()) {
        usage_error("--require-signed requires at least one --verify-pubkey");
    }
    return opts;
}

// -----------------------------------------------------------------------
// Frame reader abstraction
// -----------------------------------------------------------------------

// Format helpers
// -----------------------------------------------------------------------

string flags_str(uint64_t flags) {
    string s;
    if (has_frame_flag_end(flags))
        s += 'E';
    if (has_frame_flag_clean_end(flags))
        s += 'C';
    if (has_frame_flag_signed(flags))
        s += '!';
    if (s.empty())
        s = ".";
    return s;
}

string channel_abbrev(ChannelType t) {
    switch (t) {
    case ChannelType::CH_CONTENT:
        return "content ";
    case ChannelType::CH_METADATA:
        return "metadata";
    case ChannelType::CH_FEC:
        return "fec     ";
    case ChannelType::ARCHIVE_END:
        return "arch_end";
    }
    return "?";
}

string hash_status(const neotape::Hash &expected, const neotape::Hash &actual) {
    if (expected == actual) {
        return "OK";
    }
    return "MISMATCH";
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------

struct Stats {
    uint64_t total_frames = 0;
    uint64_t content_frames = 0;
    uint64_t metadata_frames = 0;
    uint64_t fec_frames = 0;
    uint64_t archive_end_frames = 0;
    uint64_t filemarks = 0;
    uint64_t errors = 0;
    uint64_t unsigned_frames = 0;
    uint64_t signed_frames = 0;
    uint64_t signed_unverified = 0;
    uint64_t signatures_verified = 0;
    uint64_t signature_errors = 0;
    uint64_t total_payload_bytes = 0;
};

void print_separator() {
    std::cout << "------+------+--------+----------+-----------+----------+"
                 "---------+-------+-------\n";
}

int do_inspect(const Options &opts) {
    neotape::RecordReader reader(opts.source);

    FrameValidator validator;
    // Disable hash verification for raw dump mode.
    Stats stats;
    uint64_t frame_number = 0;
    vector<string> issues;

    // --- header ---
    std::cout << format("Source: {:<40s}\n", opts.source.path);
    std::cout << "\n";
    std::cout << format("  {:>3s} | {:>4s} | {:>6s} | {:>8s} | {:>9s} | "
                        "{:>8s} | {:>7s} | {:>5s} | {:>5s}\n",
                        "#", "File", "GblSeq", "SliceSeq", "Channel",
                        "ChFrmSeq", "Payload", "Flags", "Hash");
    std::cout << "------+------+--------+----------+-----------+----------+"
                 "---------+-------+-------\n";

    for (;;) {
        auto rr = reader.next();

        if (rr.event == neotape::RecordEvent::end) {
            break;
        }

        if (rr.event == neotape::RecordEvent::filemark) {
            ++stats.filemarks;
            std::cout << format("  {:>3s} | {:>4s} | {:>6s} | {:>8s} | "
                                "{:>9s} | {:>8s} | {:>7s} | {:>5s} | {:>5s}\n",
                                "", "", "", "", "filemark", "", "", "", "");
            continue;
        }

        ++frame_number;
        ++stats.total_frames;

        // Parse header.
        const auto *data = reinterpret_cast<const uint8_t *>(rr.record.data());
        FrameHeader header;
        try {
            header = neotape::parse_fixed_header(data, rr.record.size());
        } catch (const std::exception &e) {
            ++stats.errors;
            issues.push_back(format("Frame #{}: header parse error: {}",
                                    frame_number, e.what()));
            std::cout << format(
                "  {:>3d} | {:>4d} | {:>6s} | {:>8s} | {:>9s} | {:>8s} | "
                "{:>7s} | {:>5s} | FAIL\n",
                frame_number, static_cast<int>(rr.file_num), "-", "-", "-", "-",
                "-", "-");
            continue;
        }

        uint32_t const block_size = neotape::decoded_block_size(header);

        // Validate via shared FrameValidator.
        if (frame_number == 1) {
            // A spool or tape scan may begin at any volume boundary, not
            // necessarily at archive-global frame 0 or channel-frame 0.
            validator.seed_for_stream_start(header);
        }
        auto err = validator.validate(header, data, rr.record.size());
        if (err.has_value()) {
            ++stats.errors;
            issues.push_back(format("Frame #{}: {}", frame_number, *err));
        }

        // Verify frame_hash explicitly for display.
        neotape::Hash const computed =
            neotape::compute_frame_hash(data, rr.record.size());

        bool const record_size_ok = rr.record.size() == block_size;
        if (!record_size_ok) {
            ++stats.errors;
            issues.push_back(
                format("Frame #{}: record size {} != block size {}",
                       frame_number, rr.record.size(), block_size));
        }

        stats.total_payload_bytes += header.frame_payload_size;
        switch (header.channel_type) {
        case ChannelType::CH_CONTENT:
            ++stats.content_frames;
            break;
        case ChannelType::CH_METADATA:
            ++stats.metadata_frames;
            break;
        case ChannelType::CH_FEC:
            ++stats.fec_frames;
            break;
        case ChannelType::ARCHIVE_END:
            ++stats.archive_end_frames;
            break;
        }
        if (has_frame_flag_signed(header.flags)) {
            ++stats.signed_frames;
        } else {
            ++stats.unsigned_frames;
        }
        neotape::FrameSignatureValidation const signature_validation =
            neotape::validate_frame_signature(header, opts.verify_keys,
                                              opts.require_signed);
        if (signature_validation.error.has_value()) {
            ++stats.errors;
            ++stats.signature_errors;
            issues.push_back(format("Frame #{}: {}", frame_number,
                                    *signature_validation.error));
        } else if (signature_validation.status ==
                   neotape::FrameSignatureStatus::signed_unverified) {
            ++stats.signed_unverified;
        } else if (signature_validation.status ==
                   neotape::FrameSignatureStatus::verified) {
            ++stats.signatures_verified;
        }

        std::cout << format(
            "  {:>3d} | {:>4d} | {:>6d} | {:>8d} | {:>9s} | {:>8d} | "
            "{:>7d} | {:>5s} | {:>5s}\n",
            frame_number, static_cast<int>(rr.file_num),
            header.global_frame_seq_num, header.slice_seq_num,
            channel_abbrev(header.channel_type), header.channel_frame_seq_num,
            header.frame_payload_size, flags_str(header.flags),
            hash_status(header.frame_hash, computed));
    }

    print_separator();

    // --- summary ---
    std::cout << format("\nSummary:\n");
    std::cout << format("  Total frames:     {}\n", stats.total_frames);
    std::cout << format("  Content frames:   {}\n", stats.content_frames);
    std::cout << format("  Metadata frames:  {}\n", stats.metadata_frames);
    std::cout << format("  FEC frames:       {}\n", stats.fec_frames);
    std::cout << format("  Archive_end:      {}\n", stats.archive_end_frames);
    std::cout << format("  Filemarks:        {}\n", stats.filemarks);
    std::cout << format("  Unsigned frames:  {}\n", stats.unsigned_frames);
    std::cout << format("  Signed frames:    {}\n", stats.signed_frames);
    std::cout << format("  Signed unverified: {}\n", stats.signed_unverified);
    std::cout << format("  Signatures valid: {}\n", stats.signatures_verified);
    std::cout << format("  Signature errors: {}\n", stats.signature_errors);

    if (validator.saw_any_frame && !validator.archive_uuid.empty()) {
        std::cout << format("  Archive UUID:     {}\n", validator.archive_uuid);
        std::cout << format("  Archive label:    \"{}\"\n",
                            validator.archive_label);
    }
    if (validator.volume_block_size > 0) {
        std::cout << format("  Volume block:     {} bytes\n",
                            validator.volume_block_size);
    }
    std::cout << format("  Total payload:    {} bytes\n",
                        stats.total_payload_bytes);

    // --- issues ---
    if (issues.empty()) {
        std::cout << format("\nCompliance: PASS\n");
    } else {
        std::cout << format("\nCompliance: FAIL ({} issue{})\n", issues.size(),
                            issues.size() == 1 ? "" : "s");
        for (const string &iss : issues) {
            std::cout << format("  - {}\n", iss);
        }
    }

    return issues.empty() ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        for (const string &path : opts.verify_pubkey_paths) {
            opts.verify_keys.push_back(neotape::load_signify_public_key(path));
        }
        neotape::g_debug = opts.debug;
        return do_inspect(opts);
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
