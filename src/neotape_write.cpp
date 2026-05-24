#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/tape_writer.hpp"

#include <blake3.h>

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
#include <string>
#include <string_view>
#include <vector>

namespace {

// ====================== Writer State =============================

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
};

struct WriterState {
    Options opts;
    string archive_uuid;
    uint64_t volume_seq_num = 0;
    uint64_t tape_seq = 0;
    uint64_t tape_file_num = 0;
    uint64_t volume_used = 0;
    uint64_t logical_slice_seq_num = 0;
    uint64_t global_frame_seq_num = 0;
    uint64_t frame_seq_num_within_slice = 0;
    uint64_t current_slice_size = 0;
    bool slice_open = false;
    blake3_hasher slice_hasher;
    fs::path current_volume_dir;
    fs::path current_slice_path;
    nlohmann::json manifest_files = nlohmann::json::array();
};

// ====================== Diagnostics & CLI ========================

[[noreturn]] void fail(const string &message) {
    std::cerr << format("neotape-write: {}\n", message);
    std::exit(1);
}

[[noreturn]] void fail_errno(const string &context) {
    fail(format("{}: {}", context, std::strerror(errno)));
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} -f <tape-device> [-i <input>] [options]\n"
        "       {} --target=spool -o <dir> [-i <input>] [options]\n"
        "\n"
        "Tape options:\n"
        "  -f <device>       Tape device path (implies tape mode)\n"
        "  --init            Write from BOT (overwrites)\n"
        "  --init-if-blank   Only init if tape is blank\n"
        "  --force-append    Append even without valid tail\n"
        "\n"
        "Spool options:\n"
        "  -o <dir>          Spool output directory\n"
        "\n"
        "Common options:\n"
        "  -i <input>        Payload input file (default: stdin)\n"
        "  --payload-profile <profile>  raw (default) or pax\n"
        "  --archive-name <name>\n"
        "  --volume-block-size <bytes>\n"
        "  --slice-size <bytes>\n"
        "  --virtual-tape-size <bytes>\n"
        "\n"
        "For --payload-profile=pax, provide source paths as positional args.\n",
        prog, prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"target", required_argument, nullptr, 't'},
        {"archive-name", required_argument, nullptr, 'n'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"slice-size", required_argument, nullptr, 's'},
        {"virtual-tape-size", required_argument, nullptr, 'z'},
        {"payload-profile", required_argument, nullptr, 259},
        {"init", no_argument, nullptr, 256},
        {"init-if-blank", no_argument, nullptr, 257},
        {"force-append", no_argument, nullptr, 258},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    bool saw_target = false;

    int c;
    while ((c = getopt_long(argc, argv, "f:i:o:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 't':
            if (string_view(optarg) != "spool")
                fail("only --target=spool is supported");
            saw_target = true;
            break;
        case 'f':
            opts.tape_device = optarg;
            break;
        case 'i':
            opts.input = optarg;
            break;
        case 'o':
            opts.output_dir = optarg;
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 's':
            opts.slice_size = neotape::parse_size(optarg, "slice size");
            opts.slice_size_set = true;
            break;
        case 'z':
            opts.virtual_tape_size =
                neotape::parse_size(optarg, "virtual volume size");
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case 256:
            opts.init_mode = true;
            break;
        case 257:
            opts.init_if_blank = true;
            break;
        case 258:
            opts.force_append = true;
            break;
        case 259:
            opts.payload_profile = optarg;
            break;
        case '?':
            std::exit(2);
        }
    }

    if (optind < argc && opts.input == "-")
        opts.input = argv[optind];

    if (opts.payload_profile != "raw" && opts.payload_profile != "pax")
        fail("unsupported payload profile (use raw or pax)");

    if (!saw_target && opts.output_dir.empty() && opts.tape_device.empty())
        fail("specify -f <device> (tape) or --target=spool -o <dir>");
    if (!opts.tape_device.empty() && !opts.output_dir.empty())
        fail("specify either -f (tape) or -o (spool), not both");
    if (!opts.tape_device.empty() && opts.virtual_tape_size > 0)
        fail("--virtual-tape-size is for spool mode only");
    if (saw_target && !opts.output_dir.empty())
        opts.tape_device.clear();
    if (!neotape::valid_block_size(opts.volume_block_size))
        fail("volume block size must be between 4096 and 8388608 bytes");
    if (opts.slice_size_set && opts.slice_size == 0)
        fail("slice size must be greater than zero");
    if (opts.slice_size_set && opts.slice_size < opts.volume_block_size)
        fail("slice size must be at least one volume block");
    if (opts.virtual_tape_size != 0 &&
        opts.virtual_tape_size <
            static_cast<uint64_t>(opts.volume_block_size) * 2)
        fail("virtual volume size must fit at least a volume header and one "
             "record");
    return opts;
}

string six(uint64_t value) { return format("{:06}", value); }

fs::path tape_file_path(WriterState &state, string_view suffix) {
    ++state.tape_file_num;
    return state.current_volume_dir /
           format("tape-file-{}.{}.ntf", six(state.tape_file_num), suffix);
}

// ====================== Record Files =============================

void write_record(const fs::path &path, const neotape::HeaderBytes &header,
                  const vector<uint8_t> *payload, uint32_t block_size,
                  bool append) {
    std::ofstream out(path, std::ios::binary | (append ? std::ios::app
                                                       : std::ios::openmode{}));
    if (!out)
        fail(format("open {}", path.string()));
    out.write(reinterpret_cast<const char *>(header.data()), header.size());
    if (!out)
        fail(format("write {}", path.string()));
    size_t payload_size = payload == nullptr ? 0 : payload->size();
    if (payload != nullptr && payload_size > 0)
        out.write(reinterpret_cast<const char *>(payload->data()),
                  payload_size);
    if (!out)
        fail(format("write {}", path.string()));

    vector<char> zeros(64 * 1024, 0);
    uint64_t remaining = block_size - neotape::fixed_header_size - payload_size;
    while (remaining > 0) {
        size_t n =
            static_cast<size_t>(std::min<uint64_t>(remaining, zeros.size()));
        out.write(zeros.data(), n);
        if (!out)
            fail(format("write {}", path.string()));
        remaining -= n;
    }
}

void append_manifest_file(WriterState &state, const fs::path &path) {
    state.manifest_files.push_back(nlohmann::json{
        {"tape_file_num", state.tape_file_num},
        {"path",
         path.lexically_relative(state.current_volume_dir).generic_string()},
    });
}

// ====================== Volume Management ========================

void write_tape_manifest(WriterState &state) {
    fs::path path = state.current_volume_dir / "manifest.json";

    nlohmann::json archives = nlohmann::json::array();
    if (fs::exists(path)) {
        std::ifstream in(path);
        if (in) {
            try {
                nlohmann::json j;
                in >> j;
                if (j.contains("archives") && j["archives"].is_array())
                    archives = j["archives"].get<nlohmann::json::array_t>();
            } catch (...) {
            }
        }
    }

    nlohmann::json entry;
    entry["archive_uuid"] = state.archive_uuid;
    entry["archive_name"] = state.opts.archive_name;
    entry["volume_seq_num"] = state.volume_seq_num;
    entry["files"] = state.manifest_files;
    archives.push_back(std::move(entry));

    nlohmann::json j;
    j["archives"] = std::move(archives);

    std::ofstream out(path);
    if (!out)
        fail(format("open {}", path.string()));
    out << j.dump(2) << "\n";
}

// ====================== Volume Management ========================

void finalize_current_slice_file(WriterState &state) {
    if (state.current_slice_path.empty())
        return;
    append_manifest_file(state, state.current_slice_path);
    state.current_slice_path.clear();
}

void write_volume_header(WriterState &state, bool reuse_dir = false) {
    // Finalize previous tape on rollover (skip for first call or reuse)
    if (state.tape_seq > 0 && !reuse_dir) {
        write_tape_manifest(state);
        state.manifest_files = nlohmann::json::array();
    }

    ++state.volume_seq_num;
    if (!reuse_dir) {
        ++state.tape_seq;
        state.tape_file_num = 0;
        state.volume_used = 0;
        state.current_volume_dir =
            state.opts.output_dir / format("tape-{}", six(state.tape_seq));
        fs::create_directories(state.current_volume_dir);
    }

    neotape::VolumeHeader header;
    header.volume_block_size = state.opts.volume_block_size;
    header.archive_uuid = state.archive_uuid;
    header.archive_name = state.opts.archive_name;
    header.volume_seq_num = state.volume_seq_num;
    header.payload_profile = state.opts.payload_profile == "pax"
                                 ? neotape::PayloadProfile::pax
                                 : neotape::PayloadProfile::raw;
    header.volume_write_at_utc = neotape::utc_timestamp_now();

    fs::path path = tape_file_path(state, "volume-header");
    write_record(path, neotape::serialize_volume_header(header), nullptr,
                 state.opts.volume_block_size, false);
    state.volume_used += state.opts.volume_block_size;
    append_manifest_file(state, path);
}

void ensure_room_for_record(WriterState &state) {
    if (state.opts.virtual_tape_size == 0)
        return;
    if (state.volume_used + state.opts.volume_block_size <=
        state.opts.virtual_tape_size)
        return;
    finalize_current_slice_file(state);
    write_volume_header(state);
}

// ====================== Content Framing ==========================

void write_content_frame(WriterState &state, const vector<uint8_t> &payload,
                         bool end) {
    ensure_room_for_record(state);

    if (!state.slice_open) {
        ++state.logical_slice_seq_num;
        state.frame_seq_num_within_slice = 0;
        state.current_slice_size = 0;
        state.slice_open = true;
        blake3_hasher_init(&state.slice_hasher);
    }
    if (state.current_slice_path.empty())
        state.current_slice_path = tape_file_path(
            state, format("slice-{}", six(state.logical_slice_seq_num)));

    ++state.global_frame_seq_num;
    ++state.frame_seq_num_within_slice;
    neotape::Hash payload_hash =
        neotape::blake3_hash(payload.data(), payload.size());
    blake3_hasher_update(&state.slice_hasher, payload.data(), payload.size());
    state.current_slice_size += payload.size();

    neotape::Hash slice_hash{};
    if (end)
        blake3_hasher_finalize(&state.slice_hasher, slice_hash.data(),
                               slice_hash.size());

    // Only the terminal frame of a logical slice carries the whole-slice
    // digest. Earlier frames stay independently verifiable by payload hash.
    neotape::FrameHeader header;
    header.volume_block_size = state.opts.volume_block_size;
    header.archive_uuid = state.archive_uuid;
    header.archive_name = state.opts.archive_name;
    header.volume_seq_num = state.volume_seq_num;
    header.logical_slice_seq_num = state.logical_slice_seq_num;
    header.global_frame_seq_num = state.global_frame_seq_num;
    header.frame_seq_num_within_slice = state.frame_seq_num_within_slice;
    header.frame_payload_size = payload.size();
    header.frame_payload_blake3 = payload_hash;
    uint16_t flags = 0;
    if (state.frame_seq_num_within_slice == 1)
        flags |= neotape::frame_flag_start;
    if (end) {
        header.slice_content_size = state.current_slice_size;
        header.slice_content_blake3 = slice_hash;
        flags |= neotape::frame_flag_end;
    }
    header.flags = flags;

    write_record(state.current_slice_path,
                 neotape::serialize_frame_header(header), &payload,
                 state.opts.volume_block_size, true);
    state.volume_used += state.opts.volume_block_size;

    if (end) {
        state.slice_open = false;
        finalize_current_slice_file(state);
    }
}

void write_archive_end(WriterState &state) {
    ensure_room_for_record(state);

    neotape::ArchiveEndHeader header;
    header.volume_block_size = state.opts.volume_block_size;
    header.archive_uuid = state.archive_uuid;
    header.archive_name = state.opts.archive_name;
    header.volume_seq_num = state.volume_seq_num;
    header.last_logical_slice_seq_num = state.logical_slice_seq_num;
    header.last_global_frame_seq_num = state.global_frame_seq_num;
    header.created_by_implementation = "NeoTape reference writer phase2-mvp";
    header.archive_end_at_utc = neotape::utc_timestamp_now();

    fs::path path = tape_file_path(state, "archive-end");
    write_record(path, neotape::serialize_archive_end_header(header), nullptr,
                 state.opts.volume_block_size, false);
    state.volume_used += state.opts.volume_block_size;
    append_manifest_file(state, path);
}

void write_stream_payload(WriterState &state, FILE *input, bool split_slices) {
    size_t frame_payload_capacity =
        state.opts.volume_block_size - neotape::fixed_header_size;
    vector<uint8_t> buffer(frame_payload_capacity);
    vector<uint8_t> pending;
    bool have_pending = false;

    for (;;) {
        if (have_pending && split_slices &&
            state.current_slice_size + pending.size() >=
                state.opts.slice_size) {
            write_content_frame(state, pending, true);
            pending.clear();
            have_pending = false;
            continue;
        }

        size_t want = buffer.size();
        if (split_slices) {
            uint64_t pending_size = have_pending ? pending.size() : 0;
            uint64_t used = state.slice_open ? state.current_slice_size : 0;
            uint64_t remaining = state.opts.slice_size - used - pending_size;
            want = static_cast<size_t>(
                std::min<uint64_t>(buffer.size(), remaining));
        }

        size_t n = std::fread(buffer.data(), 1, want, input);
        if (n > 0) {
            if (have_pending) {
                write_content_frame(state, pending, false);
                pending.clear();
                have_pending = false;
            }
            pending.assign(buffer.begin(),
                           buffer.begin() + static_cast<std::ptrdiff_t>(n));
            have_pending = true;
        }
        if (n != want) {
            if (std::ferror(input))
                fail_errno("read input");
            break;
        }
    }

    if (have_pending)
        write_content_frame(state, pending, true);
}

// ====================== Spool Writer Pipeline ====================

uint64_t find_last_tape_seq(const fs::path &root) {
    uint64_t last = 0;
    if (!fs::exists(root))
        return 0;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (!entry.is_directory())
            continue;
        auto name = entry.path().filename().string();
        if (name.rfind("tape-", 0) != 0)
            continue;
        char *end = nullptr;
        uint64_t num = std::strtoull(name.c_str() + 5, &end, 10);
        if (end != nullptr && *end == '\0' && num > last)
            last = num;
    }
    return last;
}

struct TapeDirInfo {
    uint64_t last_file_num = 0;
    uint64_t used_bytes = 0;
};

TapeDirInfo scan_tape_dir(const fs::path &dir) {
    TapeDirInfo info;
    if (!fs::exists(dir))
        return info;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        auto name = entry.path().filename().string();
        if (name.rfind("tape-file-", 0) != 0)
            continue;
        auto dot = name.find('.', 10);
        if (dot == string::npos)
            continue;
        char *end = nullptr;
        uint64_t num = std::strtoull(name.c_str() + 10, &end, 10);
        if (end == nullptr || *end != '.')
            continue;
        if (num > info.last_file_num)
            info.last_file_num = num;
        info.used_bytes += entry.file_size();
    }
    return info;
}

void write_spool_archive(const Options &opts) {
    FILE *input = stdin;
    if (opts.input != "-") {
        input = std::fopen(opts.input.c_str(), "rb");
        if (input == nullptr)
            fail_errno(string("open ") + opts.input);
    }

    WriterState state;
    state.opts = opts;

    bool is_append = fs::exists(opts.output_dir);
    if (is_append) {
        uint64_t last_seq = find_last_tape_seq(opts.output_dir);
        state.tape_seq = last_seq;

        if (last_seq > 0 && opts.virtual_tape_size > 0) {
            TapeDirInfo last_info = scan_tape_dir(
                opts.output_dir / format("tape-{}", six(last_seq)));
            if (last_info.used_bytes +
                    static_cast<uint64_t>(opts.volume_block_size) * 2 <=
                opts.virtual_tape_size) {
                state.tape_file_num = last_info.last_file_num;
                state.volume_used = last_info.used_bytes;
                state.current_volume_dir =
                    opts.output_dir / format("tape-{}", six(last_seq));
            }
        }
    } else {
        fs::create_directories(opts.output_dir);
    }

    state.archive_uuid = neotape::make_uuid_v4();
    bool reuse_dir = is_append && !state.current_volume_dir.empty();
    write_volume_header(state, reuse_dir);

    write_stream_payload(state, input,
                         opts.payload_profile == "raw" && opts.slice_size_set);
    if (input != stdin && std::fclose(input) != 0)
        fail_errno(string("close ") +
                   (opts.input.empty() ? "input" : opts.input));
    write_archive_end(state);
    write_tape_manifest(state);
    std::cerr << format("archive {} written to {}\n", state.archive_uuid,
                        opts.output_dir.string());
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
        mt::write_tape_archive(tape_opts);
    } else {
        write_spool_archive(opts);
    }
}

} // namespace

int neotape_write_legacy_main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        run_writer(std::move(opts));
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}

#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) { return neotape_write_legacy_main(argc, argv); }
#endif
