#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/plan.hpp"
#include "neotape/signature.hpp"
#include "neotape/validate.hpp"

#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <streambuf>
#include <string>
#include <vector>

namespace {

using neotape::fail;
using neotape::usage_error;

using neotape::ChannelType;
using neotape::CheckedFrame;
using neotape::FrameHeader;
using neotape::MediaLocator;
using neotape::RecordEvent;
using std::format;
using std::string;
using std::vector;

enum class OutputMode { raw, list, summary };

struct Options {
    MediaLocator source;
    std::optional<string> plan_path;
    OutputMode mode = OutputMode::raw;
    std::optional<string> output_path;
    bool require_signed = false;
    vector<string> verify_pubkey_paths;
    vector<neotape::SignifyPublicKey> verify_keys;
};

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} -s|--source <spool:./dir|tape:/dev/nst0> | -p|--plan "
        "<file>\n"
        "       [-l|--list | -u|--summary] [-o|--output <file>]\n"
        "       [-k|--verify-pubkey <file.pub>]... [-S|--require-signed]\n"
        "       [-h|--help]\n"
        "Prints the archive catalog (the plan file) unchanged unless --list\n"
        "or --summary is given. A tape source is read from its current\n"
        "position and is not rewound.\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"plan", required_argument, nullptr, 'p'},
        {"list", no_argument, nullptr, 'l'},
        {"summary", no_argument, nullptr, 'u'},
        {"output", required_argument, nullptr, 'o'},
        {"verify-pubkey", required_argument, nullptr, 'k'},
        {"require-signed", no_argument, nullptr, 'S'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    bool list = false;
    bool summary = false;
    int c = 0;
    while ((c = getopt_long(argc, argv, "s:p:luo:k:Sh", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 's':
            opts.source = neotape::parse_media(optarg);
            break;
        case 'p':
            opts.plan_path = optarg;
            break;
        case 'l':
            list = true;
            break;
        case 'u':
            summary = true;
            break;
        case 'o':
            opts.output_path = optarg;
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

    bool const has_source = opts.source.kind != MediaLocator::none;
    if (has_source == opts.plan_path.has_value())
        usage_error("exactly one of --source and --plan is required");
    if (list && summary)
        usage_error("--list and --summary are mutually exclusive");
    if (!has_source &&
        (opts.require_signed || !opts.verify_pubkey_paths.empty()))
        usage_error("signature options require --source");
    if (opts.require_signed && opts.verify_pubkey_paths.empty())
        usage_error("--require-signed requires at least one --verify-pubkey");
    if (optind != argc)
        usage_error("unexpected positional arguments");
    opts.mode = list      ? OutputMode::list
                : summary ? OutputMode::summary
                          : OutputMode::raw;
    return opts;
}

// Presents the leading ch_metadata stream of an archive as a byte stream.
// Reading stops at the stream's END frame, leaving a tape positioned right
// after it; nothing beyond the catalog is read.
class CatalogStreambuf : public std::streambuf {
  public:
    explicit CatalogStreambuf(const Options &opts)
        : opts_(opts), reader_(opts.source) {}

  protected:
    int_type underflow() override {
        while (!ended_) {
            load_frame();
            if (gptr() != egptr())
                return traits_type::to_int_type(*gptr());
        }
        return traits_type::eof();
    }

  private:
    void load_frame() {
        auto media = reader_.next();
        if (media.event == RecordEvent::filemark && !saw_frame_)
            return;
        if (media.event != RecordEvent::record)
            throw std::runtime_error(
                saw_frame_ ? "catalog ends before its END frame; a catalog "
                             "that continues on another volume is not "
                             "supported"
                           : "no NeoTape frame found");
        record_ = std::move(media.record);
        auto const *data = reinterpret_cast<const uint8_t *>(record_.data());
        if (!saw_frame_ && !neotape::has_frame_magic(data, record_.size())) {
            // Recovery bundle or other foreign tape file ahead of the archive.
            reader_.skip_file();
            return;
        }

        CheckedFrame const frame = neotape::check_frame(data, record_.size());
        FrameHeader const &header = frame.header;
        if (!saw_frame_) {
            if (header.global_frame_seq_num != 0)
                throw std::runtime_error(
                    format("this is not the start of an archive (global frame "
                           "{}); the catalog is at the start of the first "
                           "volume",
                           header.global_frame_seq_num));
            if (header.channel_type != ChannelType::CH_METADATA)
                throw std::runtime_error("archive has no catalog");
            saw_frame_ = true;
        } else if (header.channel_type != ChannelType::CH_METADATA) {
            throw std::runtime_error("catalog ends before its END frame");
        }
        if (auto error = validator_.validate(frame, data))
            throw std::runtime_error(*error);
        auto const signature = neotape::validate_frame_signature(
            header, opts_.verify_keys, opts_.require_signed);
        if (signature.error)
            throw std::runtime_error(*signature.error);
        if (signature.status ==
                neotape::FrameSignatureStatus::signed_unverified &&
            !warned_unverified_) {
            std::cerr << "neotape-catalog: warning: signed frames are not "
                         "authenticated because no public key is configured\n";
            warned_unverified_ = true;
        }

        ended_ = neotape::has_frame_flag_end(header.flags);
        char *const payload = reinterpret_cast<char *>(record_.data()) +
                              neotape::fixed_header_size;
        setg(payload, payload, payload + header.frame_payload_size);
    }

    const Options &opts_;
    neotape::RecordReader reader_;
    neotape::FrameValidator validator_;
    vector<std::byte> record_;
    bool saw_frame_ = false;
    bool ended_ = false;
    bool warned_unverified_ = false;
};

string format_mtime(int64_t mtime) {
    std::time_t const time = static_cast<std::time_t>(mtime);
    std::tm local{};
    char text[32];
    if (localtime_r(&time, &local) == nullptr ||
        std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local) == 0)
        return std::to_string(mtime);
    return text;
}

string owner_name(const string &name, uint32_t id) {
    return name.empty() ? std::to_string(id)
                        : neotape::escape_bytes_for_diagnostic(name);
}

void print_list(neotape::PlanReader &records, std::ostream &output) {
    while (auto record = records.next()) {
        if (!record->entry)
            continue;
        const auto &e = *record->entry;
        output << format("{:>6} {} {:>14} {} {}/{} {}\n", e.slice, e.kind,
                         e.size, format_mtime(e.mtime),
                         owner_name(e.uname, e.uid), owner_name(e.gname, e.gid),
                         neotape::escape_bytes_for_diagnostic(e.path));
    }
}

// Slices are contiguous in a plan, so one running total is enough.
void print_summary(neotape::PlanReader &records, std::ostream &output) {
    uint64_t slices = 0, total_entries = 0, total_bytes = 0;
    uint64_t slice = 0, entries = 0, bytes = 0;
    auto const flush_slice = [&] {
        if (entries == 0)
            return;
        output << format("{:>6} {:>12} {:>16}\n", slice, entries, bytes);
        ++slices;
        total_entries += entries;
        total_bytes += bytes;
    };
    output << format("{:>6} {:>12} {:>16}\n", "slice", "entries", "bytes");
    while (auto record = records.next()) {
        if (!record->entry)
            continue;
        const auto &e = *record->entry;
        if (e.slice != slice) {
            flush_slice();
            slice = e.slice;
            entries = bytes = 0;
        }
        ++entries;
        bytes += e.size;
    }
    flush_slice();
    output << format("total: slices={} entries={} bytes={}\n", slices,
                     total_entries, total_bytes);
}

void run(const Options &opts, std::istream &input, const string &input_name,
         std::ostream &output) {
    switch (opts.mode) {
    case OutputMode::raw: {
        vector<char> buffer(1024UL * 1024UL);
        while (input.read(buffer.data(),
                          static_cast<std::streamsize>(buffer.size())) ||
               input.gcount() > 0)
            output.write(buffer.data(), input.gcount());
        if (input.bad())
            throw std::runtime_error("read " + input_name + " failed");
        break;
    }
    case OutputMode::list: {
        neotape::PlanReader records(input, input_name);
        print_list(records, output);
        break;
    }
    case OutputMode::summary: {
        neotape::PlanReader records(input, input_name);
        print_summary(records, output);
        break;
    }
    }
    output.flush();
    if (!output)
        throw std::runtime_error("write output failed");
}

} // namespace

int main(int argc, char **argv) {
    neotape::program_name = "neotape-catalog";
    try {
        neotape::ensure_utf8_ctype_locale();
        Options opts = parse_args(argc, argv);
        for (const string &path : opts.verify_pubkey_paths)
            opts.verify_keys.push_back(neotape::load_signify_public_key(path));

        std::ofstream output_file;
        if (opts.output_path) {
            output_file.open(*opts.output_path, std::ios::binary);
            if (!output_file)
                fail(format("open {}: cannot write", *opts.output_path));
        }
        std::ostream &output = opts.output_path ? output_file : std::cout;

        if (opts.plan_path) {
            std::ifstream plan(*opts.plan_path, std::ios::binary);
            if (!plan)
                fail(format("open {}: cannot read", *opts.plan_path));
            run(opts, plan, *opts.plan_path, output);
        } else {
            CatalogStreambuf buffer(opts);
            std::istream catalog(&buffer);
            // Frame errors must reach the catch below, not turn into EOF.
            catalog.exceptions(std::ios::badbit);
            run(opts, catalog, "catalog", output);
        }
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
