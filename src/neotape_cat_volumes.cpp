#include "neotape/cli.hpp"
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/reader.hpp"
#include "neotape/restore_validation.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_navigator.hpp"

#include <blake3.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <nlohmann-json/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ====================== Output Sink Abstraction ===================

namespace fs = std::filesystem;
using std::format;
using std::size_t;
using std::string;
using std::string_view;

class OutputSink {
  public:
    virtual void write(const uint8_t *data, size_t len) = 0;
    virtual void flush() = 0;
    virtual ~OutputSink() = default;
};

class StdoutSink : public OutputSink {
  public:
    void write(const uint8_t *data, size_t len) override {
        if (std::fwrite(data, 1, len, stdout) != len)
            throw std::runtime_error("write to stdout failed");
    }
    void flush() override { std::fflush(stdout); }
};

class FileSink : public OutputSink {
    std::FILE *file_ = nullptr;

  public:
    explicit FileSink(const string &path) {
        file_ = std::fopen(path.c_str(), "wb");
        if (file_ == nullptr)
            throw std::runtime_error(
                format("open {}: {}", path, std::strerror(errno)));
    }
    ~FileSink() override {
        if (file_ != nullptr)
            std::fclose(file_);
    }
    void write(const uint8_t *data, size_t len) override {
        if (std::fwrite(data, 1, len, file_) != len)
            throw std::runtime_error(format("write: {}", std::strerror(errno)));
    }
    void flush() override { std::fflush(file_); }
};

// ====================== CLI Options ===============================

struct Options {
    string spool_dir;
    string output = "-";
    bool list_mode = false;
    bool prompt = false;
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
};

struct RawReadOptions {
    neotape::Locator source;
    string output = "-";
    string archive_selector;
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-cat-volumes: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage:\n"
        "  {} [options] <spool-dir>\n"
        "  {} --list [options] <spool-dir>\n"
        "\n"
        "options:\n"
        "  -o <path>        Output path (default: stdout; FIFO supported)\n"
        "  --prompt         Prompt between volumes\n"
        "  --list           List archives instead of extracting\n"
        "  -h, --help\n",
        prog, prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"output", required_argument, nullptr, 'o'},
        {"prompt", no_argument, nullptr, 256},
        {"list", no_argument, nullptr, 257},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "o:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'o':
            opts.output = optarg;
            break;
        case 256:
            opts.prompt = true;
            break;
        case 257:
            opts.list_mode = true;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        std::exit(2);
    }
    opts.spool_dir = argv[optind++];

    if (!fs::is_directory(opts.spool_dir))
        fail(format("not a directory: {}", opts.spool_dir));
    return opts;
}

void raw_read_usage() {
    std::cerr << "usage: neotape read --source <locator> --output <file|-> "
                 "[--archive <index|uuid>] [--control=auto|none]\n";
}

RawReadOptions parse_raw_read_args(int argc, char **argv,
                                   bool allow_tape = false) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"output", required_argument, nullptr, 'o'},
        {"archive", required_argument, nullptr, 'a'},
        {"control", required_argument, nullptr, 'c'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    RawReadOptions opts;
    bool saw_source = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = neotape::parse_locator(optarg);
            saw_source = true;
            break;
        case 'o':
            opts.output = optarg;
            break;
        case 'a':
            opts.archive_selector = optarg;
            break;
        case 'c':
            opts.control = neotape::parse_control_policy(optarg);
            break;
        case 'h':
            raw_read_usage();
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (!saw_source)
        fail("read requires --source <locator>");
    if (optind != argc)
        fail("read does not accept positional arguments");
    if (opts.source.kind != "spool" &&
        !(allow_tape && opts.source.kind == "tape"))
        fail("read currently supports spool: sources");
    if (!opts.archive_selector.empty() && opts.archive_selector != "1")
        fail("read currently supports only the first archive");
    return opts;
}

// ====================== Output Factory ============================

std::unique_ptr<OutputSink> create_sink(const Options &opts) {
    if (opts.output == "-")
        return std::make_unique<StdoutSink>();
    return std::make_unique<FileSink>(opts.output);
}

void append_pax_eoa_if_needed(const string &output) {
    static const uint8_t zeros[1024] = {0};
    if (output == "-") {
        if (std::fwrite(zeros, 1, sizeof(zeros), stdout) != sizeof(zeros))
            throw std::runtime_error(
                "write pax end-of-archive to stdout failed");
        return;
    }

    std::FILE *file = std::fopen(output.c_str(), "ab");
    if (file == nullptr)
        throw std::runtime_error(
            format("open {}: {}", output, std::strerror(errno)));
    if (std::fwrite(zeros, 1, sizeof(zeros), file) != sizeof(zeros)) {
        std::fclose(file);
        throw std::runtime_error("write pax end-of-archive failed");
    }
    std::fclose(file);
}

// ====================== Spool Orchestrator ========================

class SpoolOrchestrator {
    fs::path root_;
    bool consumed_ = false;

  public:
    explicit SpoolOrchestrator(const fs::path &root) : root_(root) {}
    const fs::path &root() const { return root_; }

    std::unique_ptr<neotape::SpoolVolumeReader> next_volume() {
        if (consumed_)
            return nullptr;
        consumed_ = true;
        return std::make_unique<neotape::SpoolVolumeReader>(root_);
    }
};

// ====================== Volume Read State =========================

struct VolumeReadState {
    OutputSink *sink;
    neotape::RestoreValidationState validation;
    blake3_hasher slice_hasher;
};

// ====================== Volume Processing =========================

bool process_volume(neotape::VirtualTapeReader &vol, VolumeReadState &rs,
                    const neotape::VolumeHeader &volume_header) {
    std::vector<uint8_t> record;
    neotape::accept_restore_volume_header(volume_header, rs.validation);

    while (vol.next_file()) {
        while (vol.read_record(record)) {
            auto parsed =
                neotape::parse_fixed_header(record.data(), record.size());

            if (parsed.volume) {
                neotape::accept_restore_volume_header(*parsed.volume,
                                                      rs.validation);
                continue;
            }

            if (parsed.frame) {
                auto &f = *parsed.frame;
                neotape::validate_restore_frame_header(f, rs.validation);

                if (f.frame_content_type ==
                    neotape::FrameContentType::slice_content) {
                    bool start = (f.flags & neotape::frame_flag_start) != 0;
                    bool end = (f.flags & neotape::frame_flag_end) != 0;

                    if (start) {
                        if (rs.validation.slice_open)
                            fail(format("new slice {} starts before previous "
                                        "slice ended",
                                        f.logical_slice_seq_num));
                        blake3_hasher_init(&rs.slice_hasher);
                        rs.validation.current_slice_size = 0;
                    }
                    if (!rs.validation.slice_open && !start)
                        fail(format(
                            "content frame outside slice at global frame {}",
                            f.global_frame_seq_num));

                    auto payload_offset = neotape::fixed_header_size;
                    auto payload_size =
                        static_cast<size_t>(f.frame_payload_size);

                    rs.sink->write(record.data() + payload_offset,
                                   payload_size);

                    blake3_hasher_update(&rs.slice_hasher,
                                         record.data() + payload_offset,
                                         payload_size);

                    if (end) {
                        neotape::Hash slice_hash{};
                        blake3_hasher_finalize(&rs.slice_hasher,
                                               slice_hash.data(),
                                               slice_hash.size());

                        uint64_t slice_size = rs.validation.current_slice_size +
                                              f.frame_payload_size;
                        if (f.slice_content_size != slice_size)
                            fail(format(
                                "slice {} size mismatch: declared {} actual {}",
                                f.logical_slice_seq_num, f.slice_content_size,
                                slice_size));
                        if (f.slice_content_blake3 != slice_hash)
                            fail(format("slice {} BLAKE3 mismatch",
                                        f.logical_slice_seq_num));
                    }
                }

                neotape::note_restore_frame_accepted(f, rs.validation);
            }

            if (parsed.archive_end) {
                auto &ae = *parsed.archive_end;
                neotape::validate_restore_archive_end(ae, rs.validation);
                return true;
            }
        }
    }

    return false;
}

// ====================== List Archives =============================

struct ArchiveEntry {
    string uuid;
    string name;
    string profile;
    string status = "incomplete";
    uint64_t block_size = 0;
    uint64_t total_frames = 0;
    int volume_count = 0;
};

bool parse_spool_tape_file_num(const fs::path &path, uint64_t &num) {
    static constexpr string_view prefix = "tape-file-";
    static constexpr string_view ext = ".nts";
    auto name = path.filename().string();
    if (name.size() <= prefix.size() + ext.size() ||
        name.rfind(prefix, 0) != 0 ||
        name.substr(name.size() - ext.size()) != ext)
        return false;
    auto dot = name.find('.', prefix.size());
    if (dot == string::npos)
        return false;
    string num_str = name.substr(prefix.size(), dot - prefix.size());
    char *end = nullptr;
    num = std::strtoull(num_str.c_str(), &end, 10);
    return end != nullptr && *end == '\0';
}

std::vector<fs::path> scan_spool_tape_files(const fs::path &root) {
    std::vector<std::pair<uint64_t, fs::path>> indexed;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        uint64_t num = 0;
        if (parse_spool_tape_file_num(entry.path(), num))
            indexed.emplace_back(num, entry.path());
    }
    std::ranges::sort(indexed, {}, &std::pair<uint64_t, fs::path>::first);

    std::vector<fs::path> files;
    files.reserve(indexed.size());
    for (auto &item : indexed)
        files.push_back(std::move(item.second));
    return files;
}

std::vector<uint8_t> read_fixed_header_prefix(const fs::path &path) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        throw std::runtime_error(
            format("open {}: {}", path.string(), std::strerror(errno)));
    std::vector<uint8_t> bytes(neotape::fixed_header_size);
    if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        std::fclose(file);
        throw std::runtime_error(format("short read from {}", path.string()));
    }
    std::fclose(file);
    return bytes;
}

std::vector<ArchiveEntry> collect_archives(SpoolOrchestrator &orch) {
    std::vector<ArchiveEntry> entries;
    std::optional<ArchiveEntry> current;

    for (const auto &path : scan_spool_tape_files(orch.root())) {
        auto bytes = read_fixed_header_prefix(path);
        auto parsed = neotape::parse_fixed_header(bytes.data(), bytes.size());
        if (parsed.volume) {
            if (current)
                entries.push_back(std::move(*current));
            current.emplace();
            current->uuid = parsed.volume->archive_uuid;
            current->name = parsed.volume->archive_name;
            current->profile =
                neotape::payload_profile_name(parsed.volume->payload_profile);
            current->block_size = parsed.volume->volume_block_size;
            ++current->volume_count;
        } else if (parsed.frame) {
            if (current)
                ++current->total_frames;
        } else if (parsed.archive_end && current) {
            current->status = "clean";
            entries.push_back(std::move(*current));
            current.reset();
        }
    }

    if (current)
        entries.push_back(std::move(*current));

    return entries;
}

std::vector<ArchiveEntry> collect_archives_from_tape_boundaries(
    const std::vector<mt::nav::ArchiveBoundary> &boundaries) {
    std::vector<ArchiveEntry> entries;
    entries.reserve(boundaries.size());
    for (const auto &boundary : boundaries) {
        ArchiveEntry entry;
        entry.uuid = boundary.volume_header.archive_uuid;
        entry.name = boundary.volume_header.archive_name;
        entry.profile = neotape::payload_profile_name(
            boundary.volume_header.payload_profile);
        entry.status = "clean";
        entry.block_size = boundary.volume_header.volume_block_size;
        entry.total_frames = boundary.end_header.last_global_frame_seq_num;
        entry.volume_count = 1;
        entries.push_back(std::move(entry));
    }
    return entries;
}

void print_archives_human(const std::vector<ArchiveEntry> &entries) {
    if (entries.empty()) {
        std::cout << "(no archives found)\n";
        return;
    }

    std::cout << format("{:<5}  {:<36}  {:<16}  {:<7}  {:>7}  {:>12}  {:<10}\n",
                        "INDEX", "UUID", "NAME", "PROFILE", "VOLUMES", "SIZE",
                        "STATUS");
    std::cout << string(106, '-') << '\n';
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto &e = entries[i];
        auto size_bytes = e.block_size * e.total_frames;
        std::cout << format(
            "{:<5}  {:<36}  {:<16}  {:<7}  {:>7}  {:>12}  {:<10}\n", i + 1,
            e.uuid, e.name, e.profile, e.volume_count,
            neotape::humanize_number(static_cast<size_t>(size_bytes)),
            e.status);
    }
}

void print_archives_json(const std::vector<ArchiveEntry> &entries) {
    nlohmann::json out = nlohmann::json::array();
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto &e = entries[i];
        out.push_back({{"index", i + 1},
                       {"uuid", e.uuid},
                       {"name", e.name},
                       {"profile", e.profile},
                       {"volumes", e.volume_count},
                       {"status", e.status}});
    }
    std::cout << out.dump(2) << "\n";
}

void list_archives(SpoolOrchestrator &orch) {
    print_archives_human(collect_archives(orch));
}

struct ListOptions {
    neotape::Locator source;
    bool json = false;
};

ListOptions parse_list_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"json", no_argument, nullptr, 'j'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    ListOptions opts;
    bool saw_source = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = neotape::parse_locator(optarg);
            saw_source = true;
            break;
        case 'j':
            opts.json = true;
            break;
        case 'h':
            std::cerr << "usage: neotape list --source <locator> [--json]\n";
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }
    if (!saw_source)
        fail("list requires --source <locator>");
    if (optind != argc)
        fail("list does not accept positional arguments");
    if (opts.source.kind != "spool" && opts.source.kind != "tape")
        fail("list source must be spool: or tape:");
    return opts;
}

void run_cat_volumes(const Options &opts) {
    SpoolOrchestrator orch(opts.spool_dir);

    if (opts.list_mode) {
        list_archives(orch);
        return;
    }

    auto sink = create_sink(opts);
    VolumeReadState rs{};
    rs.sink = sink.get();

    bool found_archive_end = false;

    while (auto vol = orch.next_volume()) {
        found_archive_end = process_volume(*vol, rs, vol->volume_header());
        if (found_archive_end)
            break;

        if (opts.prompt) {
            sink->flush();
            std::cerr << format("neotape-cat-volumes: end of tape-{:06}, ",
                                vol->volume_seq_num());
            std::cerr << "mount next volume and press Enter [q to quit]: ";
            std::string line;
            std::getline(std::cin, line);
            if (line == "q")
                fail("aborted by user");
        }
    }

    if (!found_archive_end) {
        neotape::require_prompt_allowed(opts.control);
        fail("archive incomplete: no Archive End Header found");
    }

    sink->flush();

    uint64_t total_volumes = 0;
    {
        SpoolOrchestrator count_orch(opts.spool_dir);
        while (count_orch.next_volume())
            ++total_volumes;
    }

    std::cerr << format(
        "neotape-cat-volumes: ok volumes={} slices={} frames={}\n",
        total_volumes, rs.validation.expected_logical_slice_seq_num - 1,
        rs.validation.expected_global_frame_seq_num - 1);
}

void run_tape_device_restore(const Options &opts, const string &locator) {
    auto device = std::make_unique<mt::TapeDevice>(locator, false);
    auto sink = create_sink(opts);
    VolumeReadState rs{};
    rs.sink = sink.get();

    bool found_archive_end = false;
    while (true) {
        {
            neotape::TapeDeviceVolumeReader vol(*device);
            found_archive_end = process_volume(vol, rs, vol.volume_header());
            if (found_archive_end)
                break;
            if (rs.validation.slice_open)
                fail("archive incomplete: volume ended with open slice");
        }

        neotape::VolumePromptRequest req;
        req.archive_uuid = rs.validation.archive_uuid;
        req.expected_volume = rs.validation.expected_volume_seq_num;
        req.current_locator = neotape::Locator{"tape", device->device_path()};
        req.write_mode = false;
        device->close();
        neotape::require_prompt_allowed(opts.control);
        auto result = neotape::prompt_for_volume_change(req);
        if (result.choice == neotape::VolumePromptChoice::abort)
            fail("volume change aborted by user");
        if (result.choice == neotape::VolumePromptChoice::change_locator) {
            if (!result.replacement_locator ||
                result.replacement_locator->kind != "tape")
                fail("replacement locator must be tape:<device>");
            device = std::make_unique<mt::TapeDevice>(
                result.replacement_locator->locator, false);
        } else {
            device->reopen();
        }
    }

    sink->flush();
    std::cerr << format(
        "neotape-cat-volumes: ok volumes=1 slices={} frames={}\n",
        rs.validation.expected_logical_slice_seq_num - 1,
        rs.validation.expected_global_frame_seq_num - 1);
}

} // namespace

// ====================== Main ======================================

int neotape_cat_volumes_legacy_main(int argc, char **argv) {
    try {
        auto opts = parse_args(argc, argv);
        run_cat_volumes(opts);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}

int neotape_read_main(int argc, char **argv) {
    try {
        auto raw = parse_raw_read_args(argc, argv);
        Options opts;
        opts.spool_dir = raw.source.locator;
        opts.output = raw.output;
        opts.control = raw.control;
        run_cat_volumes(opts);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape read: {}\n", e.what());
        return 1;
    }
}

int neotape_list_main(int argc, char **argv) {
    try {
        auto opts = parse_list_args(argc, argv);
        std::vector<ArchiveEntry> entries;
        if (opts.source.kind == "spool") {
            SpoolOrchestrator orch(opts.source.locator);
            entries = collect_archives(orch);
        } else {
            mt::TapeDevice dev(opts.source.locator, false);
            mt::nav::TapeNavigator nav(dev);
            entries = collect_archives_from_tape_boundaries(
                nav.scan_archive_instances());
        }
        if (opts.json)
            print_archives_json(entries);
        else
            print_archives_human(entries);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape list: {}\n", e.what());
        return 1;
    }
}

int neotape_restore_main(int argc, char **argv) {
    try {
        auto raw = parse_raw_read_args(argc, argv, true);
        Options opts;
        opts.output = raw.output;
        opts.control = raw.control;
        if (raw.source.kind == "tape") {
            run_tape_device_restore(opts, raw.source.locator);
        } else {
            opts.spool_dir = raw.source.locator;
            run_cat_volumes(opts);
        }
        append_pax_eoa_if_needed(opts.output);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape restore: {}\n", e.what());
        return 1;
    }
}

#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) {
    return neotape_cat_volumes_legacy_main(argc, argv);
}
#endif
