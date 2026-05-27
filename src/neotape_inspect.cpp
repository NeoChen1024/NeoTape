#include "neotape/cli.hpp"
#include "neotape/format.hpp"
#include "neotape/tape.hpp"

#include <blake3.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// ====================== Inspector State ==========================

namespace fs = std::filesystem;
using std::format;
using std::size_t;
using std::string;
using std::vector;

struct Options {
    neotape::Locator source;
    bool read_all = false;
};

struct InspectState {
    string archive_uuid;
    uint32_t volume_block_size = 0;
    uint64_t expected_volume_seq_num = 1;
    uint64_t expected_slice_seq_num = 1;
    uint64_t expected_global_frame_seq_num = 1;
    uint64_t expected_frame_seq_num_within_slice = 1;
    bool saw_archive_end_record = false;
    bool saw_archive_end = false;
    blake3_hasher slice_hasher;
    uint64_t slice_size = 0;
    bool slice_open = false;
};

struct DiagnosticState {
    uint64_t files = 0;
    uint64_t frames = 0;
    uint64_t last_global_frame_seq_num = 0;
    uint64_t last_logical_slice_seq_num = 0;
    uint64_t malformed = 0;
    uint64_t errors = 0;
    uint64_t warnings = 0;

    void frame(const neotape::FrameHeader &header) {
        ++frames;
        last_global_frame_seq_num =
            std::max(last_global_frame_seq_num, header.global_frame_seq_num);
        if (header.frame_content_type ==
            neotape::FrameContentType::slice_content)
            last_logical_slice_seq_num = std::max(last_logical_slice_seq_num,
                                                  header.logical_slice_seq_num);
    }

    void archive_end(const neotape::ArchiveEndHeader &header) {
        last_global_frame_seq_num = std::max(last_global_frame_seq_num,
                                             header.last_global_frame_seq_num);
        last_logical_slice_seq_num = std::max(
            last_logical_slice_seq_num, header.last_logical_slice_seq_num);
    }

    void error() { ++errors; }

    void malformed_error() {
        ++malformed;
        ++errors;
    }
};

// ====================== Diagnostics & File IO ====================

[[noreturn]] void fail(const string &message) {
    std::cerr << format("neotape-inspect: {}\n", message);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} [-r|--read] <spool-dir|spool:<dir>|tape:<device>>\n", prog);
}

Options parse_args(int argc, char **argv) {
    Options opts;
    static const struct option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"read", no_argument, nullptr, 'r'},
        {nullptr, 0, nullptr, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "hr", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case 'r':
            opts.read_all = true;
            break;
        case '?':
            std::exit(2);
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        std::exit(2);
    }
    string source = argv[optind++];
    if (optind != argc) {
        usage(argv[0]);
        std::exit(2);
    }
    if (source.find(':') == string::npos)
        opts.source = neotape::Locator{"spool", source};
    else
        opts.source = neotape::parse_locator(source);

    if (opts.source.kind != "spool" && opts.source.kind != "tape") {
        std::cerr
            << "neotape-inspect: inspect source must be spool: or tape:\n";
        usage(argv[0]);
        std::exit(2);
    }
    return opts;
}

vector<uint8_t> read_file_prefix(const fs::path &path, size_t max_bytes) {
    uintmax_t size = 0;
    try {
        size = fs::file_size(path);
    } catch (const fs::filesystem_error &e) {
        fail(e.what());
    }

    size_t bytes_to_read = static_cast<size_t>(
        std::min<uintmax_t>(size, static_cast<uintmax_t>(max_bytes)));
    vector<uint8_t> bytes(bytes_to_read);

    std::ifstream in(path, std::ios::binary);
    if (!in)
        fail(format("open {}", path.string()));
    if (bytes_to_read != 0) {
        in.read(reinterpret_cast<char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        if (in.gcount() != static_cast<std::streamsize>(bytes.size()))
            fail(format("short read {}", path.string()));
    }
    return bytes;
}

string quote_field(const string &value) {
    string escaped;
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            unsigned char byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || byte == 0x7f)
                escaped += format("\\x{:02x}", byte);
            else
                escaped += ch;
            break;
        }
    }
    return escaped;
}

bool report_error(DiagnosticState &diag, const string &label,
                  const string &message) {
    diag.error();
    std::cout << format("{}: error reason=\"{}\"\n", label,
                        quote_field(message));
    return false;
}

bool report_malformed(DiagnosticState &diag, const string &label,
                      const string &message, size_t bytes) {
    diag.malformed_error();
    std::cout << format("{}: malformed reason=\"{}\" bytes={}\n", label,
                        quote_field(message), bytes);
    return false;
}

std::optional<neotape::ParsedHeader>
try_parse_header(const string &label, const vector<uint8_t> &record,
                 DiagnosticState &diag) {
    if (record.size() < neotape::fixed_header_size) {
        report_malformed(diag, label, "short fixed header", record.size());
        return std::nullopt;
    }
    try {
        return neotape::parse_fixed_header(record.data(), record.size());
    } catch (const std::exception &e) {
        report_malformed(diag, label, e.what(), record.size());
        return std::nullopt;
    }
}

// ====================== Record Validation ========================

bool inspect_volume(const string &label, InspectState &state,
                    DiagnosticState &diag, const neotape::VolumeHeader &header,
                    size_t file_size) {
    if (header.volume_seq_num != state.expected_volume_seq_num)
        return report_error(diag, label,
                            format("expected volume {}, got {}",
                                   state.expected_volume_seq_num,
                                   header.volume_seq_num));
    if (!neotape::valid_block_size(header.volume_block_size))
        return report_error(diag, label, "invalid volume block size");
    if (file_size != header.volume_block_size)
        return report_error(
            diag, label, "volume header file size does not match block size");
    if (state.archive_uuid.empty()) {
        state.archive_uuid = header.archive_uuid;
        state.volume_block_size = header.volume_block_size;
    } else {
        if (header.archive_uuid != state.archive_uuid)
            return report_error(diag, label, "archive uuid mismatch");
        if (header.volume_block_size != state.volume_block_size)
            return report_error(diag, label, "volume block size changed");
    }
    std::cout << format("{}: volume version=1 crc=ok archive=\"{}\" "
                        "name=\"{}\" volume={} block={} "
                        "profile={} written=\"{}\" flags=0x{:04x}\n",
                        label, quote_field(header.archive_uuid),
                        quote_field(header.archive_name), header.volume_seq_num,
                        header.volume_block_size,
                        neotape::payload_profile_name(header.payload_profile),
                        quote_field(header.volume_write_at_utc), header.flags);
    ++state.expected_volume_seq_num;
    return true;
}

bool verify_zero_padding(const string &label, DiagnosticState &diag,
                         const vector<uint8_t> &bytes, size_t begin) {
    for (size_t i = begin; i < bytes.size(); ++i) {
        if (bytes[i] != 0)
            return report_error(diag, label,
                                format("non-zero padding at byte {}", i));
    }
    return true;
}

string frame_position_fields(std::optional<uint64_t> index,
                             std::optional<uint64_t> offset) {
    string fields;
    if (index)
        fields += format(" index={}", *index);
    if (offset)
        fields += format(" offset={}", *offset);
    return fields;
}

bool inspect_frame(const string &label, InspectState &state,
                   DiagnosticState &diag, const neotape::FrameHeader &header,
                   const vector<uint8_t> &bytes,
                   std::optional<uint64_t> index = std::nullopt,
                   std::optional<uint64_t> offset = std::nullopt) {
    if (header.archive_uuid != state.archive_uuid)
        return report_error(diag, label, "archive uuid mismatch");
    if (header.volume_block_size != state.volume_block_size)
        return report_error(diag, label, "block size mismatch");
    if (bytes.size() != header.volume_block_size)
        return report_error(diag, label,
                            "frame record size does not match block size");
    if (header.frame_payload_size >
        header.volume_block_size - neotape::fixed_header_size)
        return report_error(diag, label, "frame payload too large");
    if (header.global_frame_seq_num != state.expected_global_frame_seq_num)
        return report_error(diag, label,
                            format("expected global frame {}, got {}",
                                   state.expected_global_frame_seq_num,
                                   header.global_frame_seq_num));

    size_t payload_begin = neotape::fixed_header_size;
    size_t payload_end =
        payload_begin + static_cast<size_t>(header.frame_payload_size);
    neotape::Hash payload_hash = neotape::blake3_hash(
        bytes.data() + payload_begin, header.frame_payload_size);
    bool ok = true;
    if (payload_hash != header.frame_payload_blake3)
        ok = report_error(diag, label, "frame payload BLAKE3 mismatch");
    if (!verify_zero_padding(label, diag, bytes, payload_end))
        ok = false;

    bool start = (header.flags & neotape::frame_flag_start) != 0;
    bool end = (header.flags & neotape::frame_flag_end) != 0;
    if (header.frame_content_type == neotape::FrameContentType::slice_content) {
        // Slice digests are accumulated across content frames and verified only
        // when the END flag appears.
        if (start) {
            if (state.slice_open)
                return report_error(
                    diag, label,
                    "new slice starts before previous slice ended");
            if (header.logical_slice_seq_num != state.expected_slice_seq_num)
                return report_error(diag, label,
                                    format("expected slice {}, got {}",
                                           state.expected_slice_seq_num,
                                           header.logical_slice_seq_num));
            state.expected_frame_seq_num_within_slice = 1;
            blake3_hasher_init(&state.slice_hasher);
            state.slice_size = 0;
            state.slice_open = true;
        }
        if (!state.slice_open)
            return report_error(diag, label,
                                "content frame without open slice");
        if (header.frame_seq_num_within_slice !=
            state.expected_frame_seq_num_within_slice)
            ok = report_error(diag, label,
                              format("expected frame within slice {}, got {}",
                                     state.expected_frame_seq_num_within_slice,
                                     header.frame_seq_num_within_slice));
        blake3_hasher_update(&state.slice_hasher, bytes.data() + payload_begin,
                             header.frame_payload_size);
        state.slice_size += header.frame_payload_size;
        ++state.expected_frame_seq_num_within_slice;
        if (end) {
            neotape::Hash slice_hash{};
            blake3_hasher_finalize(&state.slice_hasher, slice_hash.data(),
                                   slice_hash.size());
            if (header.slice_content_size != state.slice_size)
                ok = report_error(diag, label, "slice size mismatch");
            if (slice_hash != header.slice_content_blake3)
                ok = report_error(diag, label, "slice BLAKE3 mismatch");
            state.slice_open = false;
            state.expected_frame_seq_num_within_slice = 1;
            ++state.expected_slice_seq_num;
        }
    }

    std::cout << format(
        "{}: frame{} version=1 crc=ok archive=\"{}\" volume={} global={} "
        "slice={} within={} payload={} type={} "
        "flags=0x{:04x}\n",
        label, frame_position_fields(index, offset),
        quote_field(header.archive_uuid), header.volume_seq_num,
        header.global_frame_seq_num, header.logical_slice_seq_num,
        header.frame_seq_num_within_slice, header.frame_payload_size,
        neotape::frame_content_type_name(header.frame_content_type),
        header.flags);
    ++state.expected_global_frame_seq_num;
    return ok;
}

bool inspect_archive_end(const string &label, InspectState &state,
                         DiagnosticState &diag,
                         const neotape::ArchiveEndHeader &header,
                         size_t file_size) {
    if (header.archive_uuid != state.archive_uuid)
        return report_error(diag, label, "archive uuid mismatch");
    if (header.volume_block_size != state.volume_block_size)
        return report_error(diag, label, "block size mismatch");
    if (file_size != header.volume_block_size)
        return report_error(diag, label,
                            "archive end file size does not match block size");
    if (state.slice_open)
        return report_error(diag, label, "archive ended with open slice");
    bool clean = (header.flags & neotape::archive_end_flag_clean_end) != 0;
    if (!clean)
        return report_error(diag, label, "CLEAN_END flag is not set");
    if (header.last_logical_slice_seq_num + 1 != state.expected_slice_seq_num)
        return report_error(diag, label, "last slice sequence mismatch");
    if (header.last_global_frame_seq_num + 1 !=
        state.expected_global_frame_seq_num)
        return report_error(diag, label, "last frame sequence mismatch");
    std::cout << format(
        "{}: archive_end version=1 crc=ok archive=\"{}\" volume={} "
        "last_slice={} last_frame={} clean={} flags=0x{:04x} ended=\"{}\" "
        "impl=\"{}\" build=\"{}\"\n",
        label, quote_field(header.archive_uuid), header.volume_seq_num,
        header.last_logical_slice_seq_num, header.last_global_frame_seq_num,
        clean, header.flags, quote_field(header.archive_end_at_utc),
        quote_field(header.created_by_implementation),
        quote_field(header.created_by_build_id));
    state.saw_archive_end = true;
    return true;
}

bool inspect_medium(const string &label, DiagnosticState &diag,
                    const neotape::MediumHeader &header, size_t file_size) {
    if (file_size != header.medium_header_block_size)
        return report_error(
            diag, label, "medium header file size does not match block size");
    std::cout << format(
        "{}: medium version=1 crc=ok uuid=\"{}\" label=\"{}\" block={} "
        "count={} initialized=\"{}\" flags=0x{:04x} impl=\"{}\" build=\"{}\"\n",
        label, quote_field(header.medium_uuid),
        quote_field(header.medium_label), header.medium_header_block_size,
        header.medium_header_block_count,
        quote_field(header.initialized_at_utc), header.flags,
        quote_field(header.created_by_implementation),
        quote_field(header.created_by_build_id));
    return true;
}

// ====================== Spool Traversal ==========================

std::optional<uint64_t> tape_file_number(const fs::path &path) {
    if (path.extension() != ".nts")
        return std::nullopt;

    string name = path.filename().string();
    static const string prefix = "tape-file-";
    if (!name.starts_with(prefix))
        return std::nullopt;

    size_t pos = prefix.size();
    if (pos == name.size() ||
        !std::isdigit(static_cast<unsigned char>(name[pos])))
        return std::nullopt;

    uint64_t number = 0;
    while (pos < name.size() &&
           std::isdigit(static_cast<unsigned char>(name[pos]))) {
        uint64_t digit = static_cast<uint64_t>(name[pos] - '0');
        if (number > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            return std::nullopt;
        number = number * 10 + digit;
        ++pos;
    }
    if (pos == name.size() || name[pos] != '.')
        return std::nullopt;
    if (pos + 1 >= name.size() - path.extension().string().size())
        return std::nullopt;
    return number;
}

void sort_tape_files(vector<fs::path> &files) {
    std::ranges::sort(files, [](const fs::path &a, const fs::path &b) {
        auto an = tape_file_number(a);
        auto bn = tape_file_number(b);
        if (an && bn && *an != *bn)
            return *an < *bn;
        return a < b;
    });
}

vector<fs::path> spool_tape_files(const fs::path &root) {
    vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (entry.is_regular_file() && tape_file_number(entry.path()))
            files.push_back(entry.path());
    }
    sort_tape_files(files);
    return files;
}

string spool_label(const fs::path &root, const fs::path &file) {
    fs::path rel = fs::relative(file, root);
    return format("spool:{}", quote_field(rel.generic_string()));
}

vector<uint8_t> read_spool_first_record(const fs::path &path) {
    return read_file_prefix(path, neotape::max_block_size);
}

uintmax_t file_size_or_fail(const fs::path &path) {
    try {
        return fs::file_size(path);
    } catch (const fs::filesystem_error &e) {
        fail(e.what());
    }
}

bool spool_file_looks_like_slice(const fs::path &path) {
    return path.filename().string().find(".slice-") != string::npos;
}

string offset_label(const string &label, uint64_t offset) {
    return format("{} offset={}", label, offset);
}

void advance_expected_frame_after_gap(InspectState &state) {
    if (state.expected_global_frame_seq_num !=
        std::numeric_limits<uint64_t>::max())
        ++state.expected_global_frame_seq_num;
    if (state.slice_open && state.expected_frame_seq_num_within_slice !=
                                std::numeric_limits<uint64_t>::max())
        ++state.expected_frame_seq_num_within_slice;
}

vector<uint8_t> first_record_bytes(const vector<uint8_t> &bytes,
                                   size_t record_size) {
    if (bytes.size() <= record_size)
        return bytes;
    return vector<uint8_t>(bytes.begin(),
                           bytes.begin() +
                               static_cast<std::ptrdiff_t>(record_size));
}

void inspect_first_record(const string &label, InspectState &state,
                          DiagnosticState &diag,
                          const vector<uint8_t> &record) {
    auto parsed = try_parse_header(label, record, diag);
    if (!parsed)
        return;
    if (parsed->medium) {
        vector<uint8_t> first = first_record_bytes(
            record, parsed->medium->medium_header_block_size);
        inspect_medium(label, diag, *parsed->medium, first.size());
    } else if (parsed->volume) {
        vector<uint8_t> first =
            first_record_bytes(record, parsed->volume->volume_block_size);
        inspect_volume(label, state, diag, *parsed->volume, first.size());
    } else if (parsed->frame) {
        diag.frame(*parsed->frame);
        vector<uint8_t> first =
            first_record_bytes(record, parsed->frame->volume_block_size);
        InspectState shallow = state;
        shallow.expected_global_frame_seq_num =
            parsed->frame->global_frame_seq_num;
        if (parsed->frame->frame_content_type ==
            neotape::FrameContentType::slice_content)
            shallow.expected_slice_seq_num =
                parsed->frame->logical_slice_seq_num;
        inspect_frame(label, shallow, diag, *parsed->frame, first);
    } else if (parsed->archive_end) {
        state.saw_archive_end_record = true;
        diag.archive_end(*parsed->archive_end);
        vector<uint8_t> first =
            first_record_bytes(record, parsed->archive_end->volume_block_size);
        InspectState shallow = state;
        shallow.slice_open = false;
        shallow.expected_slice_seq_num =
            parsed->archive_end->last_logical_slice_seq_num + 1;
        shallow.expected_global_frame_seq_num =
            parsed->archive_end->last_global_frame_seq_num + 1;
        if (inspect_archive_end(label, shallow, diag, *parsed->archive_end,
                                first.size()))
            state.saw_archive_end = true;
    } else {
        report_malformed(diag, label,
                         format("unknown header type {}",
                                neotape::header_type_name(parsed->type)),
                         record.size());
    }
}

void inspect_deep_record(const string &label, InspectState &state,
                         DiagnosticState &diag, const vector<uint8_t> &record,
                         uint64_t index, uint64_t offset) {
    string record_label = offset_label(label, offset);
    auto parsed = try_parse_header(record_label, record, diag);
    if (!parsed) {
        // A bad fixed-size record still consumes one frame position; advance so
        // later valid records do not all report stale global-frame sequence.
        advance_expected_frame_after_gap(state);
        return;
    }
    if (!parsed->frame) {
        report_malformed(diag, record_label,
                         "non-frame record inside frame file", record.size());
        advance_expected_frame_after_gap(state);
        return;
    }
    diag.frame(*parsed->frame);
    uint64_t expected_before = state.expected_global_frame_seq_num;
    inspect_frame(label, state, diag, *parsed->frame, record, index, offset);
    if (state.expected_global_frame_seq_num == expected_before &&
        parsed->frame->global_frame_seq_num >= expected_before)
        state.expected_global_frame_seq_num =
            parsed->frame->global_frame_seq_num + 1;
}

void inspect_spool_frame_file_deep(const fs::path &file, const string &label,
                                   InspectState &state, DiagnosticState &diag) {
    if (state.volume_block_size == 0) {
        report_error(diag, label, "frame appears before volume header");
        return;
    }

    uintmax_t file_size = file_size_or_fail(file);
    uint64_t block_size = state.volume_block_size;
    uint64_t complete_bytes =
        static_cast<uint64_t>(file_size / block_size * block_size);

    std::ifstream in(file, std::ios::binary);
    if (!in)
        fail(format("open {}", file.string()));

    vector<uint8_t> record(static_cast<size_t>(block_size));
    uint64_t index = 0;
    for (uint64_t offset = 0; offset < complete_bytes;
         offset += block_size, ++index) {
        in.read(reinterpret_cast<char *>(record.data()),
                static_cast<std::streamsize>(record.size()));
        if (in.gcount() != static_cast<std::streamsize>(record.size())) {
            report_malformed(
                diag, offset_label(label, offset), "short read",
                static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
            advance_expected_frame_after_gap(state);
            return;
        }
        inspect_deep_record(label, state, diag, record, index, offset);
    }

    uint64_t tail = static_cast<uint64_t>(file_size) - complete_bytes;
    if (tail != 0)
        report_malformed(diag, offset_label(label, complete_bytes),
                         "partial tail block", static_cast<size_t>(tail));
}

void inspect_spool_file_deep(const fs::path &file, const string &label,
                             InspectState &state, DiagnosticState &diag) {
    if (state.volume_block_size != 0 && spool_file_looks_like_slice(file)) {
        inspect_spool_frame_file_deep(file, label, state, diag);
        return;
    }

    size_t first_size = state.volume_block_size == 0
                            ? neotape::max_block_size
                            : static_cast<size_t>(state.volume_block_size);
    vector<uint8_t> first = read_file_prefix(file, first_size);
    auto parsed = try_parse_header(label, first, diag);
    if (!parsed)
        return;
    if (!parsed->frame) {
        inspect_first_record(label, state, diag, first);
        return;
    }
    inspect_spool_frame_file_deep(file, label, state, diag);
}

void inspect_spool(const Options &opts, InspectState &state,
                   DiagnosticState &diag) {
    fs::path spool_dir(opts.source.locator);
    if (!fs::is_directory(spool_dir))
        fail(format("{} is not a directory", spool_dir.string()));

    for (const fs::path &file : spool_tape_files(spool_dir)) {
        ++diag.files;
        string label = spool_label(spool_dir, file);
        if (opts.read_all) {
            inspect_spool_file_deep(file, label, state, diag);
        } else {
            vector<uint8_t> bytes = read_spool_first_record(file);
            inspect_first_record(label, state, diag, bytes);
        }
    }
}

std::optional<vector<uint8_t>>
read_tape_record(mt::TapeDevice &dev, size_t size, const string &label,
                 DiagnosticState &diag, bool allow_eof = false,
                 bool require_full = false) {
    vector<uint8_t> record(size);
    ssize_t n = ::read(dev.fd(), record.data(), record.size());
    if (n < 0) {
        report_error(diag, label, format("read: {}", std::strerror(errno)));
        return std::nullopt;
    }
    if (n == 0 && allow_eof)
        return vector<uint8_t>{};
    if (n == 0) {
        report_error(diag, label, "short read from tape");
        return std::nullopt;
    }
    if (require_full && static_cast<size_t>(n) < size)
        report_error(
            diag, label,
            format("short read from tape: expected {}, got {}", size, n));
    record.resize(static_cast<size_t>(n));
    return record;
}

bool advance_tape_file(mt::TapeDevice &dev, DiagnosticState &diag,
                       const string &label) {
    try {
        dev.space_fwd();
        return true;
    } catch (const mt::Error &e) {
        report_error(diag, label, e.what());
        return false;
    }
}

bool inspect_tape_file_deep(mt::TapeDevice &dev, const string &label,
                            InspectState &state, DiagnosticState &diag,
                            const vector<uint8_t> &first_record) {
    auto parsed = try_parse_header(label, first_record, diag);
    if (!parsed)
        return false;
    if (!parsed->frame) {
        inspect_first_record(label, state, diag, first_record);
        return false;
    }
    if (state.volume_block_size == 0) {
        report_error(diag, label, "frame appears before volume header");
        return false;
    }

    inspect_deep_record(label, state, diag, first_record, 0, 0);
    uint64_t index = 1;
    uint64_t offset = state.volume_block_size;
    while (true) {
        auto record = read_tape_record(dev, state.volume_block_size, label,
                                       diag, true, true);
        if (!record)
            return false;
        if (record->empty())
            return true;
        inspect_deep_record(label, state, diag, *record, index, offset);
        ++index;
        offset += state.volume_block_size;
    }
}

void inspect_tape(const Options &opts, InspectState &state,
                  DiagnosticState &diag) {
    mt::TapeDevice dev(opts.source.locator, false);
    uint64_t file_num = 0;
    size_t read_size = 8 * 1024 * 1024;

    while (true) {
        string label = format("tape:file-{}", file_num);
        auto record =
            read_tape_record(dev, read_size, label, diag, file_num != 0);
        if (!record)
            break;
        if (record->empty())
            break;
        ++diag.files;

        bool filemark_consumed = false;
        if (opts.read_all)
            filemark_consumed =
                inspect_tape_file_deep(dev, label, state, diag, *record);
        else
            inspect_first_record(label, state, diag, *record);

        if (!filemark_consumed && !advance_tape_file(dev, diag, label))
            break;
        ++file_num;
        if (state.volume_block_size != 0)
            read_size = state.volume_block_size;
    }
}

uint64_t completed_count(uint64_t next_expected) {
    return next_expected > 0 ? next_expected - 1 : 0;
}

void print_summary(const InspectState &state, const DiagnosticState &diag) {
    uint64_t frames =
        std::max(completed_count(state.expected_global_frame_seq_num),
                 diag.last_global_frame_seq_num);
    uint64_t slices = std::max(completed_count(state.expected_slice_seq_num),
                               diag.last_logical_slice_seq_num);
    std::cout << format("summary: files={} malformed={} errors={} warnings={} "
                        "archive={} volumes={} frames={} slices={} end={}\n",
                        diag.files, diag.malformed, diag.errors, diag.warnings,
                        state.archive_uuid.empty() ? string("-")
                                                   : state.archive_uuid,
                        completed_count(state.expected_volume_seq_num), frames,
                        slices, state.saw_archive_end ? "yes" : "no");
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        InspectState state;
        DiagnosticState diag;
        if (opts.source.kind == "tape")
            inspect_tape(opts, state, diag);
        else
            inspect_spool(opts, state, diag);
        print_summary(state, diag);
        return diag.errors == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
