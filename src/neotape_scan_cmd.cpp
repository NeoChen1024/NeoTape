#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/tape.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using neotape::FrameHeader;
using std::format;
using std::string;
using std::string_view;
using std::vector;

namespace fs = std::filesystem;

using SourceLocator = neotape::MediaLocator;
using neotape::parse_media;

string source_display(const SourceLocator &source) {
    switch (source.kind) {
    case SourceLocator::tape:
        return format("tape:{}", source.path);
    case SourceLocator::spool:
        return format("spool:{}", source.path);
    case SourceLocator::null_sink:
    case SourceLocator::none:
        break;
    }
    return source.path;
}

struct Options {
    SourceLocator source;
    bool verbose = false;
};

struct FirstFrame {
    uint64_t tapefile_num = 0;
    FrameHeader header;
};

struct ArchiveEntry {
    string archive_uuid;
    string archive_label;
    uint64_t first_tapefile_num = 0;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-scan: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-scan: {}\n", msg);
    std::exit(2);
}

void usage(const char *prog) {
    std::cerr << format("usage: {} -s|--source <spool:./dir|tape:/dev/nst0>\n"
                        "       [-v|--verbose] [-h|--help]\n",
                        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "s:vh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = parse_media(optarg);
            break;
        case 'v':
            opts.verbose = true;
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
    if (optind != argc) {
        usage_error("unexpected positional arguments");
    }
    return opts;
}

FrameHeader read_first_spool_frame(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(format("open {}", path.string()));
    }

    std::array<uint8_t, neotape::fixed_header_size> bytes{};
    if (!in.read(reinterpret_cast<char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error(
            format("short header read from {}", path.filename().string()));
    }

    return neotape::parse_fixed_header(bytes.data(), bytes.size());
}

template <typename Handler>
void record_first_frame(vector<string> &issues, uint64_t tapefile_num,
                        const uint8_t *data, std::size_t size,
                        string_view source_name, Handler &&handle) {
    try {
        FrameHeader const header = neotape::parse_fixed_header(data, size);
        handle(FirstFrame{tapefile_num, header});
    } catch (const std::exception &e) {
        issues.push_back(format("tapefile #{} ({}): {}", tapefile_num,
                                source_name, e.what()));
    }
}

template <typename Handler>
vector<string> scan_spool_source(const fs::path &root, Handler &&handle) {
    vector<string> issues;
    if (!fs::exists(root))
        throw std::runtime_error("spool directory does not exist: " +
                                 root.string());
    for (const auto &file : neotape::scan_spool_files(root)) {
        uint64_t file_num = 0;
        neotape::parse_spool_file_name(file, file_num);
        try {
            FrameHeader const header = read_first_spool_frame(file);
            handle(FirstFrame{file_num, header});
        } catch (const std::exception &e) {
            issues.push_back(format("tapefile #{} ({}): {}", file_num,
                                    file.filename().string(), e.what()));
        }
    }
    return issues;
}

template <typename Handler>
vector<string> scan_tape_source(const string &path, Handler &&handle) {
    vector<string> issues;
    neotape::RecordReader reader({SourceLocator::tape, path});
    for (;;) {
        auto record = reader.next();
        if (record.event == neotape::RecordEvent::end)
            break;
        if (record.event == neotape::RecordEvent::filemark)
            continue;
        record_first_frame(
            issues, record.file_num,
            reinterpret_cast<const uint8_t *>(record.record.data()),
            record.record.size(), record.source_name, handle);
        reader.skip_file();
    }
    return issues;
}

ArchiveEntry *find_archive(vector<ArchiveEntry> &archives,
                           const FrameHeader &header) {
    auto it = std::ranges::find_if(archives, [&](const ArchiveEntry &entry) {
        return entry.archive_uuid == header.archive_uuid &&
               entry.archive_label == header.archive_label;
    });
    if (it == archives.end()) {
        return nullptr;
    }
    return &*it;
}

void print_new_archive(const FirstFrame &frame) {
    std::cout << format("Archive first seen at tapefile #{}: archive_uuid={} "
                        "archive_label=\"{}\"\n",
                        frame.tapefile_num, frame.header.archive_uuid,
                        frame.header.archive_label);
}

void print_verbose_first_frame(const FirstFrame &frame, bool is_new_archive) {
    std::cout << format(
        "Tapefile #{}: channel={} global_frame_seq_num={} "
        "slice_seq_num={} channel_frame_seq_num={} archive_uuid={} "
        "archive_label=\"{}\" new_archive={}\n",
        frame.tapefile_num,
        neotape::channel_type_name(frame.header.channel_type),
        frame.header.global_frame_seq_num, frame.header.slice_seq_num,
        frame.header.channel_frame_seq_num, frame.header.archive_uuid,
        frame.header.archive_label, is_new_archive ? "yes" : "no");
}

int do_scan(const Options &opts) {
    vector<ArchiveEntry> archives;
    uint64_t tapefiles_scanned = 0;

    auto handle_first_frame = [&](const FirstFrame &frame) {
        ++tapefiles_scanned;
        bool const is_new_archive =
            find_archive(archives, frame.header) == nullptr;
        if (is_new_archive) {
            archives.push_back({frame.header.archive_uuid,
                                frame.header.archive_label,
                                frame.tapefile_num});
        }

        if (opts.verbose) {
            print_verbose_first_frame(frame, is_new_archive);
            return;
        }
        if (is_new_archive) {
            print_new_archive(frame);
        }
    };

    vector<string> issues;

    std::cout << format("Source: {}\n", source_display(opts.source));
    if (opts.source.kind == SourceLocator::spool) {
        issues =
            scan_spool_source(fs::path(opts.source.path), handle_first_frame);
    } else {
        issues = scan_tape_source(opts.source.path, handle_first_frame);
    }

    std::cout << format("Unique archives found: {}\n", archives.size());
    std::cout << format("Tapefiles scanned: {}\n", tapefiles_scanned);

    if (issues.empty()) {
        return 0;
    }

    std::cerr << format("neotape-scan: {} issue{}\n", issues.size(),
                        issues.size() == 1 ? "" : "s");
    for (const string &issue : issues) {
        std::cerr << format("  - {}\n", issue);
    }
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options const opts = parse_args(argc, argv);
        return do_scan(opts);
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
