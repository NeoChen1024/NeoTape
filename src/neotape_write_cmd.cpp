#include "neotape/cli.hpp"
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_writer.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <nlohmann-json/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using std::format;
using std::size_t;
using std::string;
using std::string_view;
using std::vector;

struct Options {
    string input = "-";
    string tape_device;
    fs::path output_dir;
    string archive_name = "raw";
    uint32_t volume_block_size = 1024 * 1024;
    uint64_t slice_size = 0;
    uint64_t virtual_tape_size = 0;
    bool slice_size_set = false;
    bool init_mode = false;
    bool init_if_blank = false;
    bool force_append = false;
    string payload_profile = "raw";
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
};

struct RawWriteOptions {
    neotape::Locator target;
    string input = "-";
    string archive_name = "raw";
    uint32_t volume_block_size = 4 * 1024 * 1024;
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
};

struct BackupOptions {
    neotape::Locator target;
    string archive_name = "pax";
    uint32_t volume_block_size = 4 * 1024 * 1024;
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
    std::optional<string> chdir_dir;
    std::optional<fs::path> plan_path;
    vector<fs::path> sources;
    size_t output_buf_size = 64UL * 1024 * 1024;
    unsigned buffer_percent = 0;
    unsigned io_thread = 1;
};

// ====================== Diagnostics & CLI ========================

[[noreturn]] void fail(const string &message) {
    std::cerr << format("neotape write: {}\n", message);
    std::exit(1);
}

[[noreturn]] void fail_errno(const string &context) {
    fail(format("{}: {}", context, std::strerror(errno)));
}

void raw_write_usage() {
    std::cerr << "usage: neotape write --target <locator> --input <file|-> "
                 "[--name <name>] [--volume-block-size <bytes>] "
                 "[--control=auto|none]\n";
}

RawWriteOptions parse_raw_write_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"target", required_argument, nullptr, 't'},
        {"input", required_argument, nullptr, 'i'},
        {"name", required_argument, nullptr, 'n'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"control", required_argument, nullptr, 'c'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    RawWriteOptions opts;
    bool saw_target = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
        case 't':
            opts.target = neotape::parse_locator(optarg);
            saw_target = true;
            break;
        case 'i':
            opts.input = optarg;
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 'c':
            opts.control = neotape::parse_control_policy(optarg);
            break;
        case 'h':
            raw_write_usage();
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (!saw_target)
        fail("write requires --target <locator>");
    if (optind != argc)
        fail("write does not accept positional arguments");
    if (opts.target.kind != "spool" && opts.target.kind != "tape")
        fail("write target must be tape: or spool:");
    if (!neotape::valid_block_size(opts.volume_block_size))
        fail("volume block size must be between 4096 and 8388608 bytes");
    return opts;
}

void backup_usage() {
    std::cerr << "usage: neotape backup --target <locator> [-C <dir>] <path> "
                 "[path ...]\n"
                 "       neotape backup --target <locator> -p <plan>\n"
                 "       [--name <name>] [--volume-block-size <bytes>] "
                 "[--control=auto|none]\n"
                 "       [-P <buffer-percent>] [-B <bytes>] [-T <N>] "
                 "[--output-buffer-size <bytes>] [--io-thread <N>]\n";
}

BackupOptions parse_backup_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"target", required_argument, nullptr, 't'},
        {"name", required_argument, nullptr, 'n'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"control", required_argument, nullptr, 'c'},
        {"plan", required_argument, nullptr, 'p'},
        {"directory", required_argument, nullptr, 'C'},
        {"buffer-percent", required_argument, nullptr, 'P'},
        {"output-buffer-size", required_argument, nullptr, 'B'},
        {"io-thread", required_argument, nullptr, 'T'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    BackupOptions opts;
    bool saw_target = false;
    bool saw_chdir = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "C:p:P:B:T:", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 't':
            opts.target = neotape::parse_locator(optarg);
            saw_target = true;
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 'c':
            opts.control = neotape::parse_control_policy(optarg);
            break;
        case 'p':
            opts.plan_path = fs::path(optarg);
            break;
        case 'C':
            if (saw_chdir)
                fail("-C may be specified at most once");
            saw_chdir = true;
            opts.chdir_dir = optarg;
            break;
        case 'P': {
            char *end = nullptr;
            unsigned long n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n > 100)
                fail("-P requires a percent from 0 to 100");
            opts.buffer_percent = static_cast<unsigned>(n);
            break;
        }
        case 'B':
            opts.output_buf_size = static_cast<size_t>(
                neotape::parse_size(optarg, "output buffer size"));
            break;
        case 'T': {
            char *end = nullptr;
            unsigned long n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0')
                fail("--io-thread requires a number");
            opts.io_thread = static_cast<unsigned>(n);
            break;
        }
        case 'h':
            backup_usage();
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    while (optind < argc)
        opts.sources.emplace_back(argv[optind++]);

    if (!saw_target)
        fail("backup requires --target <locator>");
    if (opts.target.kind != "spool" && opts.target.kind != "tape")
        fail("backup target must be tape: or spool:");
    if (opts.plan_path && (opts.chdir_dir || !opts.sources.empty()))
        fail("-p cannot be combined with -C or positional sources");
    if (!opts.plan_path && opts.sources.empty())
        fail("backup requires source paths or -p <plan>");
    if (!neotape::valid_block_size(opts.volume_block_size))
        fail("volume block size must be between 4096 and 8388608 bytes");
    return opts;
}

uint64_t read_spool_virtual_tape_size(const fs::path &root) {
    fs::path manifest_path = root / "manifest.json";
    if (!fs::is_regular_file(manifest_path))
        fail(format("spool target is not initialized: {}", root.string()));
    std::ifstream in(manifest_path);
    if (!in)
        fail(format("open {}", manifest_path.string()));
    nlohmann::json manifest;
    in >> manifest;
    return manifest.value("virtual_tape_size", 0ull);
}

void write_spool_archive(const Options &opts) {
    FILE *input = stdin;
    if (opts.input != "-") {
        input = std::fopen(opts.input.c_str(), "rb");
        if (input == nullptr)
            fail_errno(string("open ") + opts.input);
    }

    mt::TapeWriterOptions tape_opts;
    tape_opts.device = opts.output_dir.string();
    tape_opts.archive_name = opts.archive_name;
    tape_opts.volume_block_size = opts.volume_block_size;
    tape_opts.payload_profile = opts.payload_profile;
    tape_opts.init_mode = true;

    mt::SpoolTapeDevice dev(opts.output_dir, true);
    auto produce = [&](mt::TapeChunkWriter writer) {
        size_t frame_payload_capacity =
            opts.volume_block_size - neotape::fixed_header_size;
        vector<uint8_t> buffer(frame_payload_capacity);
        for (;;) {
            size_t n = std::fread(buffer.data(), 1, buffer.size(), input);
            if (n > 0)
                writer(buffer.data(), n, false);
            if (n != buffer.size()) {
                if (std::ferror(input))
                    fail_errno("read input");
                break;
            }
        }
        writer(nullptr, 0, true);
    };
    mt::write_tape_archive_from_chunks_to_device(dev, tape_opts, produce);

    if (input != stdin && std::fclose(input) != 0)
        fail_errno(string("close ") +
                   (opts.input.empty() ? "input" : opts.input));
}

void run_spool_pax_backup(const BackupOptions &backup) {
    Options opts;
    opts.output_dir = backup.target.locator;
    opts.archive_name = backup.archive_name;
    opts.volume_block_size = backup.volume_block_size;
    opts.virtual_tape_size = read_spool_virtual_tape_size(opts.output_dir);
    opts.payload_profile = "pax";

    mt::TapeWriterOptions tape_opts;
    tape_opts.device = opts.output_dir.string();
    tape_opts.archive_name = opts.archive_name;
    tape_opts.volume_block_size = opts.volume_block_size;
    tape_opts.payload_profile = "pax";
    tape_opts.control = backup.control;
    tape_opts.init_mode = true;
    std::atomic<bool> progress_paused{false};
    tape_opts.status_pause = [&](bool paused) {
        progress_paused.store(paused, std::memory_order_relaxed);
        if (paused)
            std::cerr << "\n";
    };

    auto produce = [&](mt::TapeChunkWriter writer) {
        neotape::PaxWriterOptions pax;
        pax.output_name = "-";
        pax.plan_path = backup.plan_path;
        pax.chdir_dir = backup.chdir_dir;
        pax.output_buf_size = backup.output_buf_size;
        pax.buffer_percent = backup.buffer_percent;
        pax.io_thread = backup.io_thread;
        for (const auto &source : backup.sources)
            pax.sources.push_back(source.string());

        bool slice_open = false;
        vector<uint8_t> pending_chunk;
        size_t frame_payload_capacity =
            backup.volume_block_size - neotape::fixed_header_size;
        auto flush_pending = [&](bool end_slice) {
            if (pending_chunk.empty()) {
                writer(nullptr, 0, end_slice);
                return;
            }
            writer(pending_chunk.data(), pending_chunk.size(), end_slice);
            pending_chunk.clear();
        };
        neotape::PaxWriterCallbacks callbacks;
        callbacks.begin_slice = [&](uint64_t) {
            if (slice_open)
                throw std::runtime_error("pax writer began a slice before "
                                         "ending the previous slice");
            slice_open = true;
        };
        callbacks.write_chunk = [&](neotape::PaxChunk chunk) {
            if (!slice_open)
                callbacks.begin_slice(chunk.slice);
            auto *data = reinterpret_cast<const uint8_t *>(chunk.bytes.data());
            size_t offset = 0;
            while (offset < chunk.bytes.size()) {
                if (pending_chunk.size() == frame_payload_capacity)
                    flush_pending(false);
                size_t room = frame_payload_capacity - pending_chunk.size();
                size_t take = std::min(room, chunk.bytes.size() - offset);
                pending_chunk.insert(pending_chunk.end(), data + offset,
                                     data + offset + take);
                offset += take;
            }
        };
        callbacks.end_slice = [&](uint64_t) {
            flush_pending(true);
            slice_open = false;
        };
        callbacks.progress_paused = [&] {
            return progress_paused.load(std::memory_order_relaxed);
        };

        neotape::write_pax(pax, std::move(callbacks));
        if (slice_open)
            throw std::runtime_error("pax writer ended with an open slice");
    };

    mt::SpoolTapeDevice dev(opts.output_dir, true);
    mt::write_tape_archive_from_chunks_to_device(dev, tape_opts, produce);
}

void run_tape_pax_backup(const BackupOptions &backup) {
    mt::TapeWriterOptions opts;
    opts.device = backup.target.locator;
    opts.archive_name = backup.archive_name;
    opts.volume_block_size = backup.volume_block_size;
    opts.payload_profile = "pax";
    opts.control = backup.control;
    std::atomic<bool> progress_paused{false};
    opts.status_pause = [&](bool paused) {
        progress_paused.store(paused, std::memory_order_relaxed);
        if (paused)
            std::cerr << "\n";
    };

    auto produce = [&](mt::TapeChunkWriter writer) {
        neotape::PaxWriterOptions pax;
        pax.output_name = "-";
        pax.plan_path = backup.plan_path;
        pax.chdir_dir = backup.chdir_dir;
        pax.output_buf_size = backup.output_buf_size;
        pax.buffer_percent = backup.buffer_percent;
        pax.io_thread = backup.io_thread;
        for (const auto &source : backup.sources)
            pax.sources.push_back(source.string());

        bool slice_open = false;
        vector<uint8_t> pending_chunk;
        size_t frame_payload_capacity =
            backup.volume_block_size - neotape::fixed_header_size;
        auto flush_pending = [&](bool end_slice) {
            if (pending_chunk.empty()) {
                writer(nullptr, 0, end_slice);
                return;
            }
            writer(pending_chunk.data(), pending_chunk.size(), end_slice);
            pending_chunk.clear();
        };
        neotape::PaxWriterCallbacks callbacks;
        callbacks.begin_slice = [&](uint64_t) {
            if (slice_open)
                throw std::runtime_error("pax writer began a slice before "
                                         "ending the previous slice");
            slice_open = true;
        };
        callbacks.write_chunk = [&](neotape::PaxChunk chunk) {
            if (!slice_open)
                callbacks.begin_slice(chunk.slice);
            auto *data = reinterpret_cast<const uint8_t *>(chunk.bytes.data());
            size_t offset = 0;
            while (offset < chunk.bytes.size()) {
                if (pending_chunk.size() == frame_payload_capacity)
                    flush_pending(false);
                size_t room = frame_payload_capacity - pending_chunk.size();
                size_t take = std::min(room, chunk.bytes.size() - offset);
                pending_chunk.insert(pending_chunk.end(), data + offset,
                                     data + offset + take);
                offset += take;
            }
        };
        callbacks.end_slice = [&](uint64_t) {
            flush_pending(true);
            slice_open = false;
        };
        callbacks.progress_paused = [&] {
            return progress_paused.load(std::memory_order_relaxed);
        };

        neotape::write_pax(pax, std::move(callbacks));
        if (slice_open)
            throw std::runtime_error("pax writer ended with an open slice");
    };

    mt::write_tape_archive_from_chunks(opts, produce);
}

void run_writer(Options opts) {
    if (!opts.tape_device.empty()) {
        mt::TapeWriterOptions tape_opts;
        tape_opts.device = std::move(opts.tape_device);
        tape_opts.input = std::move(opts.input);
        tape_opts.archive_name = std::move(opts.archive_name);
        tape_opts.volume_block_size = opts.volume_block_size;
        tape_opts.slice_size = opts.slice_size;
        tape_opts.slice_size_set = opts.slice_size_set;
        tape_opts.init_mode = opts.init_mode;
        tape_opts.init_if_blank = opts.init_if_blank;
        tape_opts.force_append = opts.force_append;
        tape_opts.payload_profile = opts.payload_profile;
        tape_opts.control = opts.control;
        mt::write_tape_archive(tape_opts);
    } else {
        write_spool_archive(opts);
    }
}

} // namespace

int neotape_write_main(int argc, char **argv) {
    try {
        auto raw = parse_raw_write_args(argc, argv);
        Options opts;
        opts.input = raw.input;
        opts.archive_name = raw.archive_name;
        opts.volume_block_size = raw.volume_block_size;
        opts.payload_profile = "raw";
        opts.control = raw.control;
        if (raw.target.kind == "spool") {
            opts.output_dir = raw.target.locator;
            opts.virtual_tape_size =
                read_spool_virtual_tape_size(opts.output_dir);
        } else {
            opts.tape_device = raw.target.locator;
        }
        run_writer(std::move(opts));
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape write: {}\n", e.what());
        return 1;
    }
}

int neotape_backup_main(int argc, char **argv) {
    try {
        auto backup = parse_backup_args(argc, argv);
        if (backup.target.kind == "tape")
            run_tape_pax_backup(backup);
        else
            run_spool_pax_backup(backup);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape backup: {}\n", e.what());
        return 1;
    }
}
