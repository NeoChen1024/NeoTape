#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/reader.hpp"

#include <blake3.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
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

// ====================== Output Factory ============================

std::unique_ptr<OutputSink> create_sink(const Options &opts) {
    if (opts.output == "-")
        return std::make_unique<StdoutSink>();
    return std::make_unique<FileSink>(opts.output);
}

// ====================== Spool Orchestrator ========================

class SpoolOrchestrator {
    fs::path root_;
    uint64_t next_seq_ = 1;

  public:
    explicit SpoolOrchestrator(const fs::path &root) : root_(root) {}

    std::unique_ptr<neotape::SpoolVolumeReader> next_volume() {
        auto dir = root_ / format("tape-{:06}", next_seq_);
        if (!fs::is_directory(dir))
            return nullptr;
        ++next_seq_;
        return std::make_unique<neotape::SpoolVolumeReader>(dir);
    }
};

// ====================== Volume Read State =========================

struct VolumeReadState {
    OutputSink *sink;
    uint64_t expected_global_frame_seq_num = 1;
    uint64_t expected_logical_slice_seq_num = 1;
    blake3_hasher slice_hasher;
    uint64_t current_slice_size = 0;
    bool slice_open = false;
};

// ====================== Volume Processing =========================

bool process_volume(neotape::VirtualTapeReader &vol, VolumeReadState &rs) {
    std::vector<uint8_t> record;

    while (vol.next_file()) {
        while (vol.read_record(record)) {
            auto parsed =
                neotape::parse_fixed_header(record.data(), record.size());

            if (parsed.frame) {
                auto &f = *parsed.frame;

                if (f.frame_content_type ==
                    neotape::FrameContentType::slice_content) {
                    bool start = (f.flags & neotape::frame_flag_start) != 0;
                    bool end = (f.flags & neotape::frame_flag_end) != 0;

                    if (start) {
                        if (rs.slice_open)
                            fail(format("new slice {} starts before previous "
                                        "slice ended",
                                        f.logical_slice_seq_num));
                        if (f.logical_slice_seq_num !=
                            rs.expected_logical_slice_seq_num)
                            fail(format("expected slice {}, got slice {}",
                                        rs.expected_logical_slice_seq_num,
                                        f.logical_slice_seq_num));
                        blake3_hasher_init(&rs.slice_hasher);
                        rs.current_slice_size = 0;
                        rs.slice_open = true;
                    }
                    if (!rs.slice_open)
                        fail(format(
                            "content frame outside slice at global frame {}",
                            f.global_frame_seq_num));

                    if (f.global_frame_seq_num !=
                        rs.expected_global_frame_seq_num)
                        fail(format("expected global frame {}, got {}",
                                    rs.expected_global_frame_seq_num,
                                    f.global_frame_seq_num));

                    auto payload_offset = neotape::fixed_header_size;
                    auto payload_size =
                        static_cast<size_t>(f.frame_payload_size);

                    rs.sink->write(record.data() + payload_offset,
                                   payload_size);

                    blake3_hasher_update(&rs.slice_hasher,
                                         record.data() + payload_offset,
                                         payload_size);
                    rs.current_slice_size += payload_size;

                    if (end) {
                        neotape::Hash slice_hash{};
                        blake3_hasher_finalize(&rs.slice_hasher,
                                               slice_hash.data(),
                                               slice_hash.size());

                        if (f.slice_content_size != rs.current_slice_size)
                            fail(format(
                                "slice {} size mismatch: declared {} actual {}",
                                f.logical_slice_seq_num, f.slice_content_size,
                                rs.current_slice_size));
                        if (f.slice_content_blake3 != slice_hash)
                            fail(format("slice {} BLAKE3 mismatch",
                                        f.logical_slice_seq_num));

                        rs.slice_open = false;
                        ++rs.expected_logical_slice_seq_num;
                    }
                }

                ++rs.expected_global_frame_seq_num;
            }

            if (parsed.archive_end) {
                auto &ae = *parsed.archive_end;
                if (rs.slice_open)
                    fail("archive ended with open slice");
                if (!(ae.flags & neotape::archive_end_flag_clean_end))
                    fail("archive end missing CLEAN_END flag");
                if (ae.last_global_frame_seq_num + 1 !=
                    rs.expected_global_frame_seq_num)
                    fail(format("archive end frame seq mismatch: declared {} "
                                "expected {}",
                                ae.last_global_frame_seq_num,
                                rs.expected_global_frame_seq_num - 1));
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
    uint64_t block_size = 0;
    uint64_t total_frames = 0;
    int volume_count = 0;
};

void list_archives(SpoolOrchestrator &orch) {
    std::vector<ArchiveEntry> entries;
    std::optional<ArchiveEntry> current;
    std::vector<uint8_t> record;

    while (auto vol = orch.next_volume()) {
        auto &vh = vol->volume_header();
        string vol_uuid = vh.archive_uuid;

        if (!current || current->uuid != vol_uuid) {
            if (current)
                entries.push_back(std::move(*current));
            current.emplace();
            current->uuid = vol_uuid;
            current->name = vh.archive_name;
            current->block_size = vh.volume_block_size;
        }

        ++current->volume_count;

        bool saw_end = false;
        while (vol->next_file()) {
            while (vol->read_record(record)) {
                auto parsed =
                    neotape::parse_fixed_header(record.data(), record.size());
                if (parsed.frame)
                    ++current->total_frames;
                if (parsed.archive_end) {
                    saw_end = true;
                    break;
                }
            }
            if (saw_end)
                break;
        }

        if (saw_end) {
            entries.push_back(std::move(*current));
            current.reset();
        }
    }

    if (current)
        entries.push_back(std::move(*current));

    if (entries.empty()) {
        std::cout << "(no archives found)\n";
        return;
    }

    std::cout << format("{:<36}  {:<16}  {:>7}  {:>12}\n", "UUID", "NAME",
                        "VOLUMES", "SIZE");
    std::cout << string(80, '-') << '\n';
    for (auto &e : entries) {
        auto size_bytes = e.block_size * e.total_frames;
        std::cout << format(
            "{:<36}  {:<16}  {:>7}  {:>12}\n", e.uuid, e.name, e.volume_count,
            neotape::humanize_number(static_cast<size_t>(size_bytes)));
    }
}

} // namespace

// ====================== Main ======================================

int main(int argc, char **argv) {
    auto opts = parse_args(argc, argv);

    try {
        SpoolOrchestrator orch(opts.spool_dir);

        if (opts.list_mode) {
            list_archives(orch);
            return 0;
        }

        auto sink = create_sink(opts);
        VolumeReadState rs{};
        rs.sink = sink.get();

        bool found_archive_end = false;

        while (auto vol = orch.next_volume()) {
            found_archive_end = process_volume(*vol, rs);
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

        if (!found_archive_end)
            fail("archive incomplete: no Archive End Header found");

        sink->flush();

        uint64_t total_volumes = 0;
        {
            SpoolOrchestrator count_orch(opts.spool_dir);
            while (count_orch.next_volume())
                ++total_volumes;
        }

        std::cerr << format(
            "neotape-cat-volumes: ok volumes={} slices={} frames={}\n",
            total_volumes, rs.expected_logical_slice_seq_num - 1,
            rs.expected_global_frame_seq_num - 1);

    } catch (const std::exception &e) {
        fail(e.what());
    }

    return 0;
}
