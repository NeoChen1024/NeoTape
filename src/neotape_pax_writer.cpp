#include "neotape/bounded_buffer.hpp"
#include "neotape/closable_queue.hpp"
#include "neotape/common.hpp"
#include "neotape/pax_writer.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <blake3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace neotape {

namespace {

namespace fs = std::filesystem;
using neotape::BoundedBuffer;
using std::cerr;
using std::format;
using std::size_t;
using std::string;
using std::string_view;
using std::vector;

constexpr size_t SMALL_FILE_THRESHOLD = 4UL * 1024 * 1024;
constexpr size_t PAX_ENTRY_OVERHEAD_RESERVE = 16UL * 1024;
constexpr size_t STREAM_FLUSH_THRESH = 4UL * 1024 * 1024;

// ========================== Types ==========================

using Options = PaxWriterOptions;

struct ArchiveStats {
    std::atomic<uint64_t> input_bytes{0};
    std::atomic<uint64_t> output_bytes{0};
    std::atomic<uint64_t> walked_entries{0};
    std::atomic<uint64_t> walked_entries_since_status{0};
    std::atomic<bool> done{false};
};

struct Result {
    uint64_t seq;
    std::vector<std::byte> bytes; // empty is a valid skipped/warned entry
};

struct EntryHandle {
    archive_entry *ptr = nullptr;

    EntryHandle() = default;
    explicit EntryHandle(archive_entry *entry) : ptr(entry) {}
    ~EntryHandle() { reset(); }

    EntryHandle(const EntryHandle &) = delete;
    EntryHandle &operator=(const EntryHandle &) = delete;

    EntryHandle(EntryHandle &&other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    EntryHandle &operator=(EntryHandle &&other) noexcept {
        if (this != &other) {
            reset();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    [[nodiscard]] archive_entry *get() const { return ptr; }
    archive_entry *release() {
        archive_entry *entry = ptr;
        ptr = nullptr;
        return entry;
    }
    explicit operator bool() const { return ptr != nullptr; }

    void reset(archive_entry *entry = nullptr) {
        if (ptr != nullptr) {
            archive_entry_free(ptr);
        }
        ptr = entry;
    }
};

struct FdHandle {
    int fd = -1;

    FdHandle() = default;
    explicit FdHandle(int file_fd) : fd(file_fd) {}
    ~FdHandle() { reset(); }

    FdHandle(const FdHandle &) = delete;
    FdHandle &operator=(const FdHandle &) = delete;

    FdHandle(FdHandle &&other) noexcept : fd(other.fd) { other.fd = -1; }

    FdHandle &operator=(FdHandle &&other) noexcept {
        if (this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd; }
    int release() {
        int const file_fd = fd;
        fd = -1;
        return file_fd;
    }
    explicit operator bool() const { return fd >= 0; }

    void reset(int file_fd = -1) {
        if (fd >= 0) {
            close(fd);
        }
        fd = file_fd;
    }
};

struct PipelineWorkItem {
    uint64_t seq = 0;
    EntryHandle entry;
};

enum class PipelineOrderKind : uint8_t {
    InlineBytes,
    WorkerResult,
    LargeEntry
};

struct PipelineOrderItem {
    PipelineOrderKind kind = PipelineOrderKind::InlineBytes;
    uint64_t seq = 0;
    vector<std::byte> bytes;
    EntryHandle entry;
};

struct ResultStore {
    std::mutex mtx;
    std::condition_variable cv;
    std::map<uint64_t, Result> results;
    size_t max_size = 0;
    bool closed = false;

    explicit ResultStore(size_t capacity = 0) : max_size(capacity) {}

    bool put(Result result) {
        std::unique_lock lock(mtx);
        cv.wait(lock, [&] {
            return closed || max_size == 0 || results.size() < max_size;
        });
        if (closed) {
            return false;
        }
        results.emplace(result.seq, std::move(result));
        cv.notify_all();
        return true;
    }

    std::optional<Result> take(uint64_t seq) {
        std::unique_lock lock(mtx);
        cv.wait(lock, [&] { return closed || results.contains(seq); });
        auto it = results.find(seq);
        if (it == results.end()) {
            return std::nullopt;
        }
        Result result = std::move(it->second);
        results.erase(it);
        cv.notify_all();
        return result;
    }

    void close() {
        std::scoped_lock const lock(mtx);
        closed = true;
        cv.notify_all();
    }
};

struct PlannedEntry {
    uint64_t slice = 0;
    uint64_t file_num = 0;
    char kind = '?';
    uint64_t size = 0;
    int64_t mtime = 0;
    uid_t uid = 0;
    string uname;
    gid_t gid = 0;
    string gname;
    string path;
};

struct PlanRecord {
    std::optional<string> chdir_dir;
    std::optional<PlannedEntry> entry;
};

[[noreturn]] void throw_pax_output_errno(const string &context) {
    throw std::runtime_error(format("{}: {}", context, std::strerror(errno)));
}

string pax_slice_name(const string &prefix, uint64_t slice) {
    return format("{}{:06}.pax", prefix, slice);
}

class PaxOutputSession {
  public:
    explicit PaxOutputSession(PaxLocalOutputOptions opts)
        : opts_(std::move(opts)) {}

    ~PaxOutputSession() {
        if (file_ != nullptr && owned_) {
            std::fclose(file_);
        }
    }

    PaxWriterCallbacks callbacks() {
        if (opts_.slice_output_prefix.has_value()) {
            return PaxWriterCallbacks{
                .begin_slice =
                    [&](uint64_t slice) {
                        string path =
                            pax_slice_name(*opts_.slice_output_prefix, slice);
                        file_ = std::fopen(path.c_str(), "wb");
                        if (file_ == nullptr) {
                            throw_pax_output_errno(string("open ") + path);
                        }
                        owned_ = true;
                    },
                .write_chunk =
                    [&](PaxChunk chunk) {
                        write_chunk(chunk);
                    },
                .end_slice =
                    [&](uint64_t) {
                        close_file("close slice output");
                    },
            };
        }

        if (opts_.output_path == "-") {
            file_ = stdout;
            owned_ = false;
        } else {
            file_ = std::fopen(opts_.output_path.c_str(), "wb");
            if (file_ == nullptr) {
                throw_pax_output_errno(string("open ") + opts_.output_path);
            }
            owned_ = true;
        }

        return PaxWriterCallbacks{
            .write_chunk =
                [&](PaxChunk chunk) {
                    write_chunk(chunk);
                },
        };
    }

    void finish() {
        if (opts_.slice_output_prefix.has_value()) {
            if (file_ != nullptr) {
                close_file("close slice output");
            }
            return;
        }

        if (file_ != nullptr && owned_ && std::fclose(file_) != 0) {
            file_ = nullptr;
            owned_ = false;
            throw_pax_output_errno(string("close ") + opts_.output_path);
        }

        file_ = nullptr;
        owned_ = false;
    }

    string output_target() const {
        if (opts_.slice_output_prefix.has_value()) {
            return *opts_.slice_output_prefix;
        }
        return opts_.output_path;
    }

  private:
    PaxLocalOutputOptions opts_;
    FILE *file_ = nullptr;
    bool owned_ = false;

    void write_chunk(PaxChunk chunk) {
        if (file_ == nullptr) {
            throw std::runtime_error("output file is not open");
        }
        if (std::fwrite(chunk.bytes.data(), 1, chunk.bytes.size(), file_) !=
            chunk.bytes.size()) {
            if (opts_.slice_output_prefix.has_value()) {
                throw_pax_output_errno("write slice output");
            }
            throw_pax_output_errno("write output");
        }
    }

    void close_file(const char *context) {
        if (file_ == nullptr || !owned_) {
            file_ = nullptr;
            owned_ = false;
            return;
        }
        if (std::fclose(file_) != 0) {
            file_ = nullptr;
            owned_ = false;
            throw_pax_output_errno(context);
        }
        file_ = nullptr;
        owned_ = false;
    }
};

// ── BBSink: streaming accumulator to BoundedBuffer ──

struct BBSink {
    BoundedBuffer *dest;
    ArchiveStats *stats;
    std::vector<std::byte> accum;
    bool drop_mode = false;
};

la_ssize_t bb_sink_write(archive * /*unused*/, void *client, const void *data,
                         size_t len) {
    auto *sink = static_cast<BBSink *>(client);
    if (sink->drop_mode) {
        return static_cast<la_ssize_t>(len);
    }

    const auto *bytes = static_cast<const std::byte *>(data);
    sink->accum.insert(sink->accum.end(), bytes, bytes + len);
    if (sink->accum.size() >= STREAM_FLUSH_THRESH) {
        size_t chunk_size = sink->accum.size();
        if (!sink->dest->push(std::move(sink->accum))) {
            return -1;
        }
        sink->stats->input_bytes.fetch_add(chunk_size,
                                           std::memory_order_relaxed);
        sink->accum = {};
        sink->accum.reserve(STREAM_FLUSH_THRESH);
    }
    return static_cast<la_ssize_t>(len);
}

int bb_sink_close(archive * /*unused*/, void * /*unused*/) {
    return ARCHIVE_OK;
}

string stat_rate(uint64_t bytes_per_second) {
    return neotape::humanize_number(static_cast<size_t>(bytes_per_second));
}

string stat_count_rate(uint64_t items_per_second) {
    if (items_per_second < 1000) {
        return format("{}", items_per_second);
    }
    if (items_per_second < 1000UL * 1000) {
        return format("{:.1f}k",
                      static_cast<double>(items_per_second) / 1000.0);
    }
    return format("{:.1f}M",
                  static_cast<double>(items_per_second) / (1000.0 * 1000.0));
}

void print_pax_progress(uint64_t in_rate, uint64_t out_rate, uint64_t file_rate,
                        uint64_t current_slice, uint64_t current_out,
                        size_t buffer_percent) {
    // carridge return to overwrite the previous line, but only if we're
    // not on the first line
    cerr << format("\rin @ {:>6}/s, out @ {:>6}/s, files @ {:>6}/s, "
                   "slice {:>6}, {:>6} total, buffer {:3}% full  ",
                   stat_rate(in_rate), stat_rate(out_rate),
                   stat_count_rate(file_rate), current_slice,
                   neotape::humanize_number(static_cast<size_t>(current_out)),
                   buffer_percent);
}

// ====================== Diagnostics ==========================

[[noreturn]] void throw_archive(const char *context, archive *a) {
    const char *msg = archive_error_string(a);
    throw std::runtime_error(format(
        "pax: {}{}", context, msg != nullptr ? format(": {}", msg) : string()));
}

[[noreturn]] void throw_errno(const string &context) {
    throw std::runtime_error(
        format("pax: {}: {}", context, std::strerror(errno)));
}

void check_archive_throw(int r, archive *a, const char *context) {
    if (r == ARCHIVE_OK) {
        return;
    }
    if (r == ARCHIVE_WARN) {
        const char *msg = archive_error_string(a);
        cerr << format("pax: warning: {}{}\n", context,
                       msg != nullptr ? format(": {}", msg) : string());
        return;
    }
    throw_archive(context, a);
}

void warn_archive(const char *context, archive *a) {
    const char *msg = archive_error_string(a);
    cerr << format("pax: warning: {}{}\n", context,
                   msg != nullptr ? format(": {}", msg) : string());
}

uint64_t parse_u64_field(const string &field, const fs::path &path,
                         uint64_t record_num) {
    char *end = nullptr;
    unsigned long long const value = std::strtoull(field.c_str(), &end, 10);
    if (end == field.c_str() || *end != '\0') {
        throw std::runtime_error(
            format("{}:{}: invalid numeric field", path.string(), record_num));
    }
    return static_cast<uint64_t>(value);
}

PlanRecord parse_plan_record(string_view text, const fs::path &path,
                             uint64_t record_num) {
    if (text.starts_with("/chdir/")) {
        return PlanRecord{
            .chdir_dir = string(text.substr(7)),
            .entry = std::nullopt,
        };
    }
    if (text.empty() || text.front() != '/') {
        throw std::runtime_error(
            format("{}:{}: invalid plan record", path.string(), record_num));
    }

    // 9 slash-delimited fields: slice/file_num/kind/size/mtime/uid/
    // uname/gid/gname — then the remainder is the path.
    vector<string> fields;
    size_t start = 1;
    for (size_t i = 1; i <= text.size() && fields.size() < 9; ++i) {
        if (i == text.size() || text[i] == '/') {
            fields.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (fields.size() != 9 || start > text.size()) {
        throw std::runtime_error(
            format("{}:{}: invalid entry record", path.string(), record_num));
    }

    string entry_path(text.substr(start));
    if (entry_path.empty() || fields[2].size() != 1) {
        throw std::runtime_error(
            format("{}:{}: invalid entry record", path.string(), record_num));
    }

    char const kind = fields[2][0];

    return PlanRecord{
        .chdir_dir = std::nullopt,
        .entry =
            PlannedEntry{
                .slice = parse_u64_field(fields[0], path, record_num),
                .file_num = parse_u64_field(fields[1], path, record_num),
                .kind = kind,
                .size = parse_u64_field(fields[3], path, record_num),
                .mtime = static_cast<int64_t>(
                    parse_u64_field(fields[4], path, record_num)),
                .uid = static_cast<uid_t>(
                    parse_u64_field(fields[5], path, record_num)),
                .uname = fields[6],
                .gid = static_cast<gid_t>(
                    parse_u64_field(fields[7], path, record_num)),
                .gname = fields[8],
                .path = std::move(entry_path),
            },
    };
}

vector<PlanRecord> read_plan_records(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(
            format("open {}: {}", path.string(), std::strerror(errno)));
    }
    string data((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());

    vector<PlanRecord> records;
    uint64_t record_num = 1;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find("\0\n", pos, 2);
        if (end == string::npos) {
            throw std::runtime_error(format("{}:{}: unterminated plan record",
                                            path.string(), record_num));
        }
        records.push_back(parse_plan_record(
            string_view(data.data() + pos, end - pos), path, record_num));
        pos = end + 2;
        ++record_num;
    }
    return records;
}

// ====================== Entry Formatting =====================

void mark_link_target_as_utf8(archive_entry *entry) {
    if (const char *s = archive_entry_symlink(entry); s != nullptr) {
        archive_entry_update_symlink_utf8(entry, s);
    } else if (const char *h = archive_entry_hardlink(entry); h != nullptr) {
        archive_entry_update_hardlink_utf8(entry, h);
    }
}

void copy_pathname_utf8(archive_entry *entry, const string &path) {
    archive_entry_copy_pathname(entry, path.c_str());
}

string entry_owner_name(archive_entry *entry) {
    if (const char *n = archive_entry_uname(entry); n != nullptr) {
        return n;
    }
    return std::to_string(archive_entry_uid(entry));
}

string entry_group_name(archive_entry *entry) {
    if (const char *n = archive_entry_gname(entry); n != nullptr) {
        return n;
    }
    return std::to_string(archive_entry_gid(entry));
}

string entry_timestamp(archive_entry *entry) {
    std::time_t t = archive_entry_mtime(entry);
    std::tm lt{};
    if (localtime_r(&t, &lt) == nullptr) {
        return "00000000T000000+0000";
    }
    std::array<char, 32> buf{};
    if (std::strftime(buf.data(), buf.size(), "%Y%m%dT%H%M%S%z", &lt) == 0) {
        return "00000000T000000+0000";
    }
    return {buf.data()};
}

string entry_display_path(archive_entry *entry) {
    string p = archive_entry_pathname(entry) != nullptr
                   ? archive_entry_pathname(entry)
                   : "";
    if (archive_entry_filetype(entry) == AE_IFDIR && !p.empty() &&
        p.back() != '/') {
        p += '/';
    }
    return p;
}

string entry_size_display(archive_entry *entry) {
    la_int64_t const sz = archive_entry_size(entry);
    if (sz < 0) {
        return "?";
    }
    return neotape::humanize_number(static_cast<size_t>(sz));
}

string verbose_line(archive_entry *entry) {
    string mode = archive_entry_strmode(entry);
    if (mode.size() > 10) {
        mode.resize(10);
    }
    if (archive_entry_hardlink(entry) != nullptr && !mode.empty()) {
        mode[0] = 'h';
    }
    return format("{} {:3} {:>10} {:>10} {:>6} [{}] {}", mode,
                  archive_entry_nlink(entry), entry_owner_name(entry),
                  entry_group_name(entry), entry_size_display(entry),
                  entry_timestamp(entry), entry_display_path(entry));
}

int open_entry_file(archive_entry *entry) {
    const char *src = archive_entry_sourcepath(entry);
    if (src == nullptr) {
        src = archive_entry_pathname(entry);
    }
    if (src == nullptr) {
        return -1;
    }
    int const fd = open(src, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        cerr << format("pax: warning: open {}: {}\n", src,
                       std::strerror(errno));
    }
    return fd;
}

archive_entry *planned_entry_from_path(archive *disk, const string &path) {
    EntryHandle entry(archive_entry_new());
    if (!entry) {
        throw std::runtime_error("cannot allocate entry");
    }
    copy_pathname_utf8(entry.get(), path);
    archive_entry_copy_sourcepath(entry.get(), path.c_str());
    int const r =
        archive_read_disk_entry_from_file(disk, entry.get(), -1, nullptr);
    if (r == ARCHIVE_FATAL) {
        throw_archive("read filesystem", disk);
    }
    if (r < ARCHIVE_OK) {
        warn_archive("read filesystem", disk);
    }
    copy_pathname_utf8(entry.get(), path);
    archive_entry_copy_sourcepath(entry.get(), path.c_str());
    mark_link_target_as_utf8(entry.get());
    return entry.release();
}

// ====================== Data Copying =========================

void copy_file_data(archive *writer, archive_entry *entry, int fd) {
    const char *src = archive_entry_sourcepath(entry);
    if (src == nullptr) {
        src = archive_entry_pathname(entry);
    }
    if (src == nullptr) {
        throw_archive("entry has no source path", writer);
    }

    thread_local vector<char> buf;
    if (buf.empty()) {
        buf.resize(SMALL_FILE_THRESHOLD);
    }
    for (;;) {
        ssize_t const n = read(fd, buf.data(), buf.size());
        if (n < 0) {
            throw_errno(string("read ") + src);
        }
        if (n == 0) {
            break;
        }
        ssize_t const w =
            archive_write_data(writer, buf.data(), static_cast<size_t>(n));
        if (w < 0) {
            throw_archive("write file data", writer);
        }
        if (w != n) {
            throw std::runtime_error(
                format("pax: short archive write for {}", src));
        }
    }
}

// ====================== Drop-Mode Writer Helpers ============

struct BufCtx {
    vector<std::byte> buf;
    bool drop = false;
};

int drop_open(archive * /*unused*/, void * /*unused*/) { return ARCHIVE_OK; }

la_ssize_t drop_write(archive * /*unused*/, void *client, const void *data,
                      size_t len) {
    auto *ctx = static_cast<BufCtx *>(client);
    if (ctx->drop) {
        return static_cast<la_ssize_t>(len);
    }
    const auto *bytes = static_cast<const std::byte *>(data);
    ctx->buf.insert(ctx->buf.end(), bytes, bytes + len);
    return static_cast<la_ssize_t>(len);
}

int drop_close(archive * /*unused*/, void * /*unused*/) { return ARCHIVE_OK; }

struct ArchiveWriteHandle {
    archive *ptr = nullptr;

    explicit ArchiveWriteHandle(archive *writer) : ptr(writer) {}
    ~ArchiveWriteHandle() {
        if (ptr != nullptr) {
            archive_write_free(ptr);
        }
    }

    ArchiveWriteHandle(const ArchiveWriteHandle &) = delete;
    ArchiveWriteHandle &operator=(const ArchiveWriteHandle &) = delete;

    [[nodiscard]] archive *get() const { return ptr; }
};

vector<std::byte> serialize_entry(archive_entry *entry, int fd) {
    ArchiveWriteHandle const writer(archive_write_new());
    archive *a = writer.get();
    if (a == nullptr) {
        throw std::runtime_error("pax: cannot allocate archive writer");
    }
    check_archive_throw(archive_write_add_filter_none(a), a,
                        "set uncompressed");
    check_archive_throw(archive_write_set_format_pax(a), a, "set pax format");
    check_archive_throw(
        archive_write_set_options(a, "xattrheader=ALL,hdrcharset=UTF-8"), a,
        "set options");
    check_archive_throw(archive_write_set_bytes_per_block(a, 512), a,
                        "set block size");
    check_archive_throw(archive_write_set_bytes_in_last_block(a, 1), a,
                        "set last block");

    BufCtx ctx;
    la_int64_t const entry_size = archive_entry_size(entry);
    if (fd >= 0 && entry_size > 0) {
        size_t reserve_size =
            static_cast<size_t>(entry_size) + PAX_ENTRY_OVERHEAD_RESERVE;
        ctx.buf.reserve(reserve_size);
    } else {
        ctx.buf.reserve(PAX_ENTRY_OVERHEAD_RESERVE);
    }
    check_archive_throw(
        archive_write_open(a, &ctx, drop_open, drop_write, drop_close), a,
        "open per-entry writer");

    int const r = archive_write_header(a, entry);
    if (r == ARCHIVE_FATAL) {
        throw_archive("write header", a);
    }
    if (r < ARCHIVE_OK) {
        warn_archive("write header", a);
        ctx.drop = true;
        archive_write_close(a);
        return {};
    }
    if (fd >= 0) {
        copy_file_data(a, entry, fd);
    }
    check_archive_throw(archive_write_finish_entry(a), a, "finish entry");
    ctx.drop = true;
    check_archive_throw(archive_write_close(a), a, "close writer");
    return std::move(ctx.buf);
}

// ====================== Streaming to BB1 ====================

void stream_large_entry(BBSink &sink, archive_entry *entry, int fd) {
    sink.drop_mode = false;
    ArchiveWriteHandle const writer(archive_write_new());
    archive *a = writer.get();
    if (a == nullptr) {
        throw std::runtime_error("pax: cannot allocate archive writer");
    }
    check_archive_throw(archive_write_add_filter_none(a), a,
                        "set uncompressed");
    check_archive_throw(archive_write_set_format_pax(a), a, "set pax format");
    check_archive_throw(
        archive_write_set_options(a, "xattrheader=ALL,hdrcharset=UTF-8"), a,
        "set options");
    check_archive_throw(archive_write_set_bytes_per_block(a, 512), a,
                        "set block size");
    check_archive_throw(archive_write_set_bytes_in_last_block(a, 1), a,
                        "set last block");
    check_archive_throw(
        archive_write_open(a, &sink, drop_open, bb_sink_write, bb_sink_close),
        a, "open streaming writer");

    int const r = archive_write_header(a, entry);
    if (r == ARCHIVE_FATAL) {
        throw_archive("write header", a);
    }
    if (r < ARCHIVE_OK) {
        warn_archive("write header", a);
        sink.drop_mode = true;
        sink.accum.clear();
        sink.accum.reserve(STREAM_FLUSH_THRESH);
        archive_write_close(a);
        return;
    }
    copy_file_data(a, entry, fd);
    check_archive_throw(archive_write_finish_entry(a), a, "finish entry");

    if (!sink.accum.empty()) {
        size_t chunk_size = sink.accum.size();
        if (!sink.dest->push(std::move(sink.accum))) {
            throw std::runtime_error("pax: streaming output buffer closed");
        }
        sink.stats->input_bytes.fetch_add(chunk_size,
                                          std::memory_order_relaxed);
        sink.accum = {};
        sink.accum.reserve(STREAM_FLUSH_THRESH);
    }

    sink.drop_mode = true;
    archive_write_close(a);
}

// ====================== Worker Main ==========================

struct PipelineCancel {
    std::mutex mtx;
    std::exception_ptr error;
    std::atomic<bool> requested{false};

    void request(std::exception_ptr e) {
        std::scoped_lock const lock(mtx);
        if (requested.load(std::memory_order_relaxed)) {
            return;
        }
        error = std::move(e);
        requested.store(true, std::memory_order_release);
    }

    void rethrow_if_set() {
        std::exception_ptr saved;
        {
            std::scoped_lock const lock(mtx);
            saved = error;
        }
        if (saved) {
            std::rethrow_exception(saved);
        }
    }
};

void pipeline_worker_main(ClosableQueue<PipelineWorkItem> &work_queue,
                          ClosableQueue<PipelineOrderItem> &order_queue,
                          ResultStore &results, BoundedBuffer &bb1,
                          PipelineCancel &cancel) {
    for (;;) {
        auto item = work_queue.pop();
        if (!item.has_value()) {
            return;
        }
        try {
            int const fd = open_entry_file(item->entry.get());
            if (fd < 0) {
                Result result{item->seq, {}};
                if (!results.put(std::move(result))) {
                    return;
                }
                continue;
            }
            vector<std::byte> bytes = serialize_entry(item->entry.get(), fd);
            close(fd);
            Result result{item->seq, std::move(bytes)};
            if (!results.put(std::move(result))) {
                return;
            }
        } catch (...) {
            cancel.request(std::current_exception());
            work_queue.close();
            order_queue.close();
            results.close();
            bb1.close();
            return;
        }
    }
}

bool emit_bytes_to_bb1(BBSink &sink, vector<std::byte> bytes) {
    if (bytes.empty()) {
        return true;
    }
    size_t chunk_size = bytes.size();
    if (!sink.dest->push(std::move(bytes))) {
        return false;
    }
    sink.stats->input_bytes.fetch_add(chunk_size, std::memory_order_relaxed);
    return true;
}

void pipeline_serializer_main(ClosableQueue<PipelineWorkItem> &work_queue,
                              ClosableQueue<PipelineOrderItem> &order_queue,
                              ResultStore &results, BBSink &bb1_sink,
                              PipelineCancel &cancel) {
    for (;;) {
        auto item = order_queue.pop();
        if (!item.has_value()) {
            return;
        }
        try {
            bool keep_running = true;
            switch (item->kind) {
            case PipelineOrderKind::InlineBytes:
                keep_running =
                    emit_bytes_to_bb1(bb1_sink, std::move(item->bytes));
                break;
            case PipelineOrderKind::WorkerResult: {
                auto result = results.take(item->seq);
                if (!result.has_value()) {
                    keep_running = false;
                    break;
                }
                keep_running =
                    emit_bytes_to_bb1(bb1_sink, std::move(result->bytes));
                break;
            }
            case PipelineOrderKind::LargeEntry: {
                int const fd = open_entry_file(item->entry.get());
                if (fd >= 0) {
                    stream_large_entry(bb1_sink, item->entry.get(), fd);
                    close(fd);
                }
                break;
            }
            }
            if (!keep_running) {
                return;
            }
        } catch (...) {
            cancel.request(std::current_exception());
            work_queue.close();
            order_queue.close();
            results.close();
            bb1_sink.dest->close();
            return;
        }
    }
}

size_t pipeline_worker_count(const Options &opts) {
    return opts.io_thread > 0 ? opts.io_thread - 1 : 0;
}

size_t pipeline_order_queue_capacity(const Options &opts) {
    return std::max<size_t>(64, opts.io_thread * 8);
}

size_t pipeline_work_queue_capacity(const Options &opts) {
    return std::max<size_t>(1, opts.io_thread * 4);
}

size_t pipeline_result_capacity(const Options &opts) {
    return pipeline_order_queue_capacity(opts) + pipeline_worker_count(opts);
}

class PaxPipeline {
  public:
    PaxPipeline(const Options &opts, PaxWriterCallbacks &callbacks,
                ArchiveStats &stats, blake3_hasher &hasher)
        : opts_(opts), callbacks_(callbacks), stats_(stats), hasher_(hasher),
          bb1_(opts.output_buf_size),
          order_queue_(pipeline_order_queue_capacity(opts)),
          work_queue_(pipeline_work_queue_capacity(opts)),
          results_(pipeline_result_capacity(opts)),
          bb1_sink_{&bb1_, &stats_, {}, false} {}

    ~PaxPipeline() noexcept {
        if (!started_ || joined_) {
            return;
        }
        try {
            request_cancel(std::make_exception_ptr(
                std::runtime_error("pax pipeline destroyed before join")));
            join_threads(false);
        } catch (...) {
        }
    }

    void start() {
        if (joined_) {
            throw std::runtime_error("pax pipeline cannot start after join");
        }
        if (started_ && !joined_) {
            throw std::runtime_error("pax pipeline already started");
        }

        started_ = true;
        try {
            output_thread_ = std::thread([this] { output_main(); });
            auto const nworkers =
                static_cast<unsigned>(pipeline_worker_count(opts_));
            for (unsigned i = 0; i < nworkers; ++i) {
                workers_.emplace_back(
                    pipeline_worker_main, std::ref(work_queue_),
                    std::ref(order_queue_), std::ref(results_), std::ref(bb1_),
                    std::ref(cancel_));
            }
            serializer_thread_ =
                std::thread(pipeline_serializer_main, std::ref(work_queue_),
                            std::ref(order_queue_), std::ref(results_),
                            std::ref(bb1_sink_), std::ref(cancel_));
        } catch (...) {
            request_cancel(std::current_exception());
            join_threads(false);
            throw;
        }
    }

    bool enqueue_inline(uint64_t seq, vector<std::byte> bytes) {
        PipelineOrderItem item;
        item.kind = PipelineOrderKind::InlineBytes;
        item.seq = seq;
        item.bytes = std::move(bytes);
        return order_queue_.push(std::move(item));
    }

    bool enqueue_small(uint64_t seq, EntryHandle entry) {
        if (!work_queue_.push(PipelineWorkItem{seq, std::move(entry)})) {
            return false;
        }
        PipelineOrderItem order_item;
        order_item.kind = PipelineOrderKind::WorkerResult;
        order_item.seq = seq;
        if (!order_queue_.push(std::move(order_item))) {
            request_cancel(std::make_exception_ptr(std::runtime_error(
                "pax pipeline cancelled before ordered work publish")));
            return false;
        }
        return true;
    }

    bool enqueue_large(uint64_t seq, EntryHandle entry) {
        PipelineOrderItem item;
        item.kind = PipelineOrderKind::LargeEntry;
        item.seq = seq;
        item.entry = std::move(entry);
        return order_queue_.push(std::move(item));
    }

    void finish_input() {
        work_queue_.close();
        order_queue_.close();
    }

    void request_cancel(std::exception_ptr e) {
        cancel_.request(std::move(e));
        work_queue_.close();
        order_queue_.close();
        results_.close();
        bb1_.close();
    }

    void join() { join_threads(true); }

    void rethrow_if_failed() { cancel_.rethrow_if_set(); }

    size_t buffered_bytes() const { return bb1_.size_bytes(); }
    size_t buffer_capacity() const { return bb1_.capacity_bytes(); }

  private:
    void join_threads(bool rethrow_error) {
        if (joined_) {
            return;
        }
        if (!started_) {
            joined_ = true;
            return;
        }
        finish_input();
        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        results_.close();
        if (serializer_thread_.joinable()) {
            serializer_thread_.join();
        }
        bb1_.close();
        if (output_thread_.joinable()) {
            output_thread_.join();
        }
        joined_ = true;
        if (rethrow_error) {
            cancel_.rethrow_if_set();
        }
    }

    void output_main() {
        size_t output_restart_bytes =
            opts_.output_buf_size * opts_.buffer_percent / 100;
        bool wait_for_waterline = output_restart_bytes > 0;
        try {
            for (;;) {
                auto chunk = wait_for_waterline
                                 ? bb1_.pop_after_fill(output_restart_bytes)
                                 : bb1_.pop();
                if (chunk.empty()) {
                    break;
                }
                callbacks_.write_chunk(PaxChunk{
                    .slice = 0,
                    .bytes =
                        std::span<const std::byte>(chunk.data(), chunk.size()),
                });
                blake3_hasher_update(&hasher_, chunk.data(), chunk.size());
                stats_.output_bytes.fetch_add(chunk.size(),
                                              std::memory_order_relaxed);
                wait_for_waterline =
                    output_restart_bytes > 0 && bb1_.size_bytes() == 0;
            }
        } catch (...) {
            request_cancel(std::current_exception());
        }
    }

    const Options &opts_;
    PaxWriterCallbacks &callbacks_;
    ArchiveStats &stats_;
    blake3_hasher &hasher_;
    BoundedBuffer bb1_;
    ClosableQueue<PipelineOrderItem> order_queue_;
    ClosableQueue<PipelineWorkItem> work_queue_;
    ResultStore results_;
    PipelineCancel cancel_;
    BBSink bb1_sink_;
    vector<std::thread> workers_;
    std::thread serializer_thread_;
    std::thread output_thread_;
    bool started_ = false;
    bool joined_ = false;
};

class PlannedSliceOutput {
  public:
    PlannedSliceOutput(const Options &opts, PaxWriterCallbacks &callbacks,
                       ArchiveStats &stats, blake3_hasher &hasher)
        : opts_(opts), callbacks_(callbacks), stats_(stats), hasher_(hasher) {}

    ~PlannedSliceOutput() noexcept {
        try {
            abort();
        } catch (...) {
        }
    }

    void begin_slice(uint64_t slice) {
        if (active_slice_.has_value()) {
            throw std::runtime_error("planned slice output already active");
        }
        rethrow_if_failed();

        callbacks_.begin_slice(slice);
        active_slice_ = slice;
        buffer_ = std::make_unique<BoundedBuffer>(opts_.output_buf_size);
        try {
            output_thread_ = std::thread([this, slice] { output_main(slice); });
        } catch (...) {
            buffer_.reset();
            active_slice_.reset();
            throw;
        }
    }

    void emit_entry(archive_entry *entry, int fd) {
        if (!active_slice_.has_value()) {
            throw std::runtime_error("planned slice output is not active");
        }
        rethrow_if_failed();

        la_int64_t const size = archive_entry_size(entry);
        if (fd >= 0 && std::cmp_greater(size, SMALL_FILE_THRESHOLD)) {
            BBSink sink{buffer_.get(), &stats_, {}, false};
            sink.accum.reserve(STREAM_FLUSH_THRESH);
            stream_large_entry(sink, entry, fd);
            return;
        }

        push_bytes(serialize_entry(entry, fd), true);
    }

    void emit_eoa() {
        push_bytes(vector<std::byte>(1024, std::byte{0}), false);
    }

    void finish_slice() {
        if (!active_slice_.has_value()) {
            return;
        }
        rethrow_if_failed();

        buffer_->close();
        join_output_thread();
        rethrow_if_failed();

        uint64_t const slice = *active_slice_;
        buffer_.reset();
        active_slice_.reset();
        callbacks_.end_slice(slice);
    }

    void abort() {
        if (buffer_ != nullptr) {
            buffer_->close();
        }
        join_output_thread();
        buffer_.reset();
        active_slice_.reset();
    }

    size_t buffered_bytes() const {
        if (buffer_ == nullptr) {
            return 0;
        }
        return buffer_->size_bytes();
    }

    size_t buffer_capacity() const {
        if (buffer_ == nullptr) {
            return 0;
        }
        return buffer_->capacity_bytes();
    }

  private:
    void push_bytes(vector<std::byte> bytes, bool count_input) {
        rethrow_if_failed();
        if (bytes.empty()) {
            return;
        }

        size_t const chunk_size = bytes.size();
        if (buffer_ == nullptr || !buffer_->push(std::move(bytes))) {
            rethrow_if_failed();
            throw std::runtime_error("planned output buffer closed");
        }
        if (count_input) {
            stats_.input_bytes.fetch_add(chunk_size, std::memory_order_relaxed);
        }
    }

    void output_main(uint64_t slice) {
        BoundedBuffer *buffer = buffer_.get();
        size_t const output_restart_bytes =
            opts_.output_buf_size * opts_.buffer_percent / 100;
        bool wait_for_waterline = output_restart_bytes > 0;

        try {
            for (;;) {
                auto chunk =
                    wait_for_waterline
                        ? buffer->pop_after_fill(output_restart_bytes)
                        : buffer->pop();
                if (chunk.empty()) {
                    break;
                }
                callbacks_.write_chunk(PaxChunk{
                    .slice = slice,
                    .bytes =
                        std::span<const std::byte>(chunk.data(), chunk.size()),
                });
                blake3_hasher_update(&hasher_, chunk.data(), chunk.size());
                stats_.output_bytes.fetch_add(chunk.size(),
                                              std::memory_order_relaxed);
                wait_for_waterline =
                    output_restart_bytes > 0 && buffer->size_bytes() == 0;
            }
        } catch (...) {
            record_failure(std::current_exception());
            buffer->close();
        }
    }

    void join_output_thread() {
        if (output_thread_.joinable()) {
            output_thread_.join();
        }
    }

    void record_failure(std::exception_ptr failure) {
        std::scoped_lock const lock(failure_mtx_);
        if (!failure_) {
            failure_ = std::move(failure);
        }
    }

    void rethrow_if_failed() {
        std::exception_ptr failure;
        {
            std::scoped_lock const lock(failure_mtx_);
            failure = failure_;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    const Options &opts_;
    PaxWriterCallbacks &callbacks_;
    ArchiveStats &stats_;
    blake3_hasher &hasher_;
    std::optional<uint64_t> active_slice_;
    std::unique_ptr<BoundedBuffer> buffer_;
    std::thread output_thread_;
    std::mutex failure_mtx_;
    std::exception_ptr failure_;
};

// ====================== Archive Emission =====================

PaxWriteResult write_planned_pax_archive(const Options &opts,
                                         PaxWriterCallbacks callbacks) {
    ensure_utf8_ctype_locale();

    if (!callbacks.write_chunk) {
        callbacks.write_chunk = [](PaxChunk) {};
    }

    if (!opts.plan_path.has_value()) {
        throw std::runtime_error("planned pax archive requires a plan path");
    }
    if (opts.chdir_dir.has_value() && chdir(opts.chdir_dir->c_str()) != 0) {
        throw_errno(string("chdir ") + *opts.chdir_dir);
    }

    vector<PlanRecord> const records = read_plan_records(*opts.plan_path);
    ArchiveStats stats;
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    PlannedSliceOutput slice_output(opts, callbacks, stats, hasher);

    std::optional<uint64_t> current_slice;
    std::atomic<uint64_t> emitted_slices{0};
    std::atomic<uint64_t> current_slice_seq{0};
    std::exception_ptr failure;
    archive *disk = nullptr;
    archive_entry_linkresolver *resolver = nullptr;

    std::thread stats_thread([&] {
        using clock = std::chrono::steady_clock;
        uint64_t last_in = 0;
        uint64_t last_out = 0;
        auto last_time = clock::now();

        while (!stats.done.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stats.done.load(std::memory_order_relaxed)) {
                break;
            }
            if (callbacks.progress_paused()) {
                continue;
            }
            auto now = clock::now();
            double seconds =
                std::chrono::duration<double>(now - last_time).count();
            if (seconds <= 0.0) {
                seconds = 1.0;
            }

            uint64_t const current_in =
                stats.input_bytes.load(std::memory_order_relaxed);
            uint64_t const current_out =
                stats.output_bytes.load(std::memory_order_relaxed);
            auto const in_rate =
                static_cast<uint64_t>((current_in - last_in) / seconds);
            auto const out_rate =
                static_cast<uint64_t>((current_out - last_out) / seconds);
            auto const file_rate =
                stats.walked_entries_since_status.exchange(
                    0, std::memory_order_relaxed);
            size_t const buffered = slice_output.buffered_bytes();
            size_t const capacity = slice_output.buffer_capacity();
            size_t const percent =
                capacity == 0
                    ? 0
                    : std::min<size_t>(100, buffered * 100 / capacity);

            print_pax_progress(
                in_rate, out_rate, file_rate,
                current_slice_seq.load(std::memory_order_relaxed), current_out,
                percent);

            last_in = current_in;
            last_out = current_out;
            last_time = now;
        }
    });

    auto emit_entry = [&](EntryHandle entry) {
        if (!entry) {
            return;
        }
        stats.walked_entries.fetch_add(1, std::memory_order_relaxed);
        stats.walked_entries_since_status.fetch_add(1,
                                                    std::memory_order_relaxed);

        if (opts.verbose > 1) {
            cerr << format("\n{}", verbose_line(entry.get()));
        } else if (opts.verbose > 0) {
            cerr << format("\na {}", entry_display_path(entry.get()));
        }

        bool const is_reg = (archive_entry_filetype(entry.get()) == AE_IFREG);
        la_int64_t const size = archive_entry_size(entry.get());
        bool const has_data = (is_reg && size > 0);
        FdHandle const fd(has_data ? open_entry_file(entry.get()) : -1);
        if (has_data && !fd) {
            return;
        }

        slice_output.emit_entry(entry.get(), fd.get());
    };

    try {
        disk = archive_read_disk_new();
        if (disk == nullptr) {
            throw std::runtime_error("cannot allocate disk reader");
        }
        check_archive_throw(archive_read_disk_set_symlink_physical(disk), disk,
                            "set physical symlink");
        check_archive_throw(archive_read_disk_set_standard_lookup(disk), disk,
                            "set uid/gid name lookup");

        resolver = archive_entry_linkresolver_new();
        if (resolver == nullptr) {
            throw std::runtime_error("cannot allocate hardlink resolver");
        }
        ArchiveWriteHandle const tmp(archive_write_new());
        if (tmp.get() == nullptr) {
            throw std::runtime_error("cannot allocate archive writer");
        }
        check_archive_throw(archive_write_add_filter_none(tmp.get()), tmp.get(),
                            "set uncompressed");
        check_archive_throw(archive_write_set_format_pax(tmp.get()), tmp.get(),
                            "set pax format");
        archive_entry_linkresolver_set_strategy(resolver,
                                                archive_format(tmp.get()));

        for (const PlanRecord &record : records) {
            if (record.chdir_dir.has_value()) {
                if (chdir(record.chdir_dir->c_str()) != 0) {
                    throw_errno(string("chdir ") + *record.chdir_dir);
                }
                continue;
            }
            if (!record.entry.has_value()) {
                continue;
            }

            const PlannedEntry &planned = *record.entry;
            if (!current_slice.has_value() || *current_slice != planned.slice) {
                if (current_slice.has_value()) {
                    slice_output.finish_slice();
                }
                current_slice = planned.slice;
                current_slice_seq.store(
                    emitted_slices.fetch_add(1, std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
                slice_output.begin_slice(*current_slice);
            }

            archive_entry *entry_raw =
                planned_entry_from_path(disk, planned.path);
            archive_entry *spare_raw = nullptr;
            archive_entry_linkify(resolver, &entry_raw, &spare_raw);
            EntryHandle entry(entry_raw);
            EntryHandle spare(spare_raw);
            emit_entry(std::move(entry));
            emit_entry(std::move(spare));
        }

        for (;;) {
            archive_entry *entry_raw = nullptr;
            archive_entry *spare_raw = nullptr;
            archive_entry_linkify(resolver, &entry_raw, &spare_raw);
            if ((entry_raw == nullptr) && (spare_raw == nullptr)) {
                break;
            }
            EntryHandle entry(entry_raw);
            EntryHandle spare(spare_raw);
            emit_entry(std::move(entry));
            emit_entry(std::move(spare));
        }

        if (current_slice.has_value()) {
            slice_output.emit_eoa();
            slice_output.finish_slice();
        }
    } catch (...) {
        failure = std::current_exception();
        slice_output.abort();
    }

    if (resolver != nullptr) {
        archive_entry_linkresolver_free(resolver);
    }
    if (disk != nullptr) {
        archive_read_free(disk);
    }
    stats.done.store(true, std::memory_order_relaxed);
    if (stats_thread.joinable()) {
        stats_thread.join();
    }

    if (failure) {
        std::rethrow_exception(failure);
    }

    print_pax_progress(0, 0, 0,
                       current_slice_seq.load(std::memory_order_relaxed),
                       stats.output_bytes.load(std::memory_order_relaxed),
                       slice_output.buffer_capacity() == 0
                           ? 0
                           : std::min<size_t>(
                                 100, slice_output.buffered_bytes() * 100 /
                                          slice_output.buffer_capacity()));
    cerr << "\n";

    std::array<uint8_t, BLAKE3_OUT_LEN> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());
    string hex;
    for (uint8_t b : hash) {
        hex += format("{:02x}", static_cast<unsigned>(b));
    }
    return PaxWriteResult{
        .input_bytes = stats.input_bytes.load(std::memory_order_relaxed),
        .output_bytes = stats.output_bytes.load(std::memory_order_relaxed),
        .walked_entries = stats.walked_entries.load(std::memory_order_relaxed),
        .slices = emitted_slices.load(std::memory_order_relaxed),
        .blake3_hex = hex,
    };
}

PaxWriteResult write_pax_archive(const Options &opts,
                                 PaxWriterCallbacks callbacks) {
    ensure_utf8_ctype_locale();

    if (opts.plan_path.has_value()) {
        return write_planned_pax_archive(opts, std::move(callbacks));
    }

    if (!callbacks.write_chunk) {
        callbacks.write_chunk = [](PaxChunk) {};
    }
    callbacks.begin_slice(0);
    uint64_t emitted_slice_count = 1;
    ArchiveStats stats;
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    PaxPipeline pipeline(opts, callbacks, stats, hasher);
    pipeline.start();

    std::thread stats_thread([&] {
        using clock = std::chrono::steady_clock;
        uint64_t last_in = 0;
        uint64_t last_out = 0;
        auto last_time = clock::now();

        while (!stats.done.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stats.done.load(std::memory_order_relaxed)) {
                break;
            }
            if (callbacks.progress_paused()) {
                continue;
            }
            auto now = clock::now();
            double seconds =
                std::chrono::duration<double>(now - last_time).count();
            if (seconds <= 0.0) {
                seconds = 1.0;
            }

            uint64_t const current_in =
                stats.input_bytes.load(std::memory_order_relaxed);
            uint64_t const current_out =
                stats.output_bytes.load(std::memory_order_relaxed);
            auto const in_rate =
                static_cast<uint64_t>((current_in - last_in) / seconds);
            auto const out_rate =
                static_cast<uint64_t>((current_out - last_out) / seconds);
            auto const file_rate =
                stats.walked_entries_since_status.exchange(
                    0, std::memory_order_relaxed);
            size_t buffered = pipeline.buffered_bytes();
            size_t capacity = pipeline.buffer_capacity();
            size_t percent =
                capacity == 0
                    ? 0
                    : std::min<size_t>(100, buffered * 100 / capacity);

            print_pax_progress(in_rate, out_rate, file_rate,
                               emitted_slice_count, current_out, percent);

            last_in = current_in;
            last_out = current_out;
            last_time = now;
        }
    });

    auto abort_setup = [&](std::exception_ptr setup_failure) {
        pipeline.request_cancel(setup_failure);
        try {
            pipeline.join();
        } catch (...) {
            if (!setup_failure) {
                setup_failure = std::current_exception();
            }
        }
        stats.done.store(true, std::memory_order_relaxed);
        if (stats_thread.joinable()) {
            stats_thread.join();
        }
        std::rethrow_exception(setup_failure);
    };

    archive_entry_linkresolver *resolver = archive_entry_linkresolver_new();
    if (resolver == nullptr) {
        abort_setup(std::make_exception_ptr(
            std::runtime_error("pax: cannot allocate hardlink resolver")));
    }
    archive *tmp = archive_write_new();
    if (tmp == nullptr) {
        archive_entry_linkresolver_free(resolver);
        abort_setup(std::make_exception_ptr(
            std::runtime_error("pax: cannot allocate archive writer")));
    }
    archive_write_add_filter_none(tmp);
    archive_write_set_format_pax(tmp);
    archive_entry_linkresolver_set_strategy(resolver, archive_format(tmp));
    archive_write_free(tmp);

    // ── Dispatch lambda ──
    uint64_t next_seq = 0;

    auto dispatch_entry = [&](archive_entry *raw_entry) {
        EntryHandle entry(raw_entry);
        if (!entry) {
            return;
        }
        stats.walked_entries.fetch_add(1, std::memory_order_relaxed);
        stats.walked_entries_since_status.fetch_add(1,
                                                    std::memory_order_relaxed);

        bool const is_reg = (archive_entry_filetype(entry.get()) == AE_IFREG);
        la_int64_t const size = archive_entry_size(entry.get());
        bool const has_data = (is_reg && size > 0);

        if (opts.verbose > 1) {
            cerr << format("\n{}", verbose_line(entry.get()));
        } else if (opts.verbose > 0) {
            cerr << format("\na {}", entry_display_path(entry.get()));
        }

        if (!has_data) {
            uint64_t const seq = next_seq++;
            vector<std::byte> bytes = serialize_entry(entry.get(), -1);
            entry.reset();
            if (!pipeline.enqueue_inline(seq, std::move(bytes))) {
                pipeline.rethrow_if_failed();
                throw std::runtime_error(
                    "pax pipeline closed while enqueueing metadata entry");
            }
            return;
        }

        uint64_t const seq = next_seq++;
        unsigned const nworkers = opts.io_thread > 0 ? opts.io_thread - 1 : 0;
        bool queued = false;
        if (nworkers == 0 || std::cmp_greater(size, SMALL_FILE_THRESHOLD)) {
            queued = pipeline.enqueue_large(seq, std::move(entry));
        } else {
            queued = pipeline.enqueue_small(seq, std::move(entry));
        }
        if (!queued) {
            pipeline.rethrow_if_failed();
            throw std::runtime_error(
                "pax pipeline closed while enqueueing file entry");
        }
    };

    std::exception_ptr failure;
    try {
        if (opts.chdir_dir.has_value() && chdir(opts.chdir_dir->c_str()) != 0) {
            throw_errno(string("chdir ") + *opts.chdir_dir);
        }

        for (const string &source_arg : opts.sources) {
            neotape::SourceSpec const spec =
                neotape::make_source_spec(source_arg);

            archive *disk = archive_read_disk_new();
            if (disk == nullptr) {
                throw std::runtime_error("pax: cannot allocate disk reader");
            }
            try {
                check_archive_throw(
                    archive_read_disk_set_symlink_physical(disk), disk,
                    "set physical symlink");
                if (opts.one_file_system) {
                    check_archive_throw(
                        archive_read_disk_set_behavior(
                            disk, ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS),
                        disk, "set one-file-system");
                }
                check_archive_throw(archive_read_disk_set_standard_lookup(disk),
                                    disk, "set uid/gid name lookup");
                check_archive_throw(
                    archive_read_disk_open(disk, spec.open_path.c_str()), disk,
                    "open source path");

                for (;;) {
                    EntryHandle entry(archive_entry_new());
                    if (!entry) {
                        throw std::runtime_error("pax: cannot allocate entry");
                    }

                    int r = archive_read_next_header2(disk, entry.get());
                    if (r == ARCHIVE_EOF) {
                        break;
                    }
                    if (r == ARCHIVE_FATAL) {
                        throw_archive("read filesystem", disk);
                    }
                    if (r < ARCHIVE_OK) {
                        warn_archive("read filesystem", disk);
                        continue;
                    }

                    if (archive_read_disk_can_descend(disk) != 0) {
                        r = archive_read_disk_descend(disk);
                        if (r == ARCHIVE_FATAL) {
                            throw_archive("descend", disk);
                        }
                        if (r < ARCHIVE_OK) {
                            warn_archive("descend", disk);
                        }
                    }

                    const char *src = archive_entry_sourcepath(entry.get());
                    if (src != nullptr) {
                        string ap = neotape::archive_path_for_source(spec, src);
                        copy_pathname_utf8(entry.get(), ap);
                    }
                    mark_link_target_as_utf8(entry.get());

                    archive_entry *raw_entry = entry.release();
                    archive_entry *spare_raw = nullptr;
                    archive_entry_linkify(resolver, &raw_entry, &spare_raw);
                    EntryHandle linked_entry(raw_entry);
                    EntryHandle spare(spare_raw);
                    dispatch_entry(linked_entry.release());
                    dispatch_entry(spare.release());
                }
            } catch (...) {
                archive_read_close(disk);
                archive_read_free(disk);
                throw;
            }
            archive_read_close(disk);
            archive_read_free(disk);
        }

        for (;;) {
            archive_entry *entry_raw = nullptr;
            archive_entry *spare_raw = nullptr;
            archive_entry_linkify(resolver, &entry_raw, &spare_raw);
            if ((entry_raw == nullptr) && (spare_raw == nullptr)) {
                break;
            }
            EntryHandle entry(entry_raw);
            EntryHandle spare(spare_raw);
            dispatch_entry(entry.release());
            dispatch_entry(spare.release());
        }

        pipeline.finish_input();
    } catch (...) {
        failure = std::current_exception();
        pipeline.request_cancel(failure);
    }

    try {
        pipeline.join();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }

    archive_entry_linkresolver_free(resolver);
    stats.done.store(true, std::memory_order_relaxed);
    if (stats_thread.joinable()) {
        stats_thread.join();
    }

    if (failure) {
        std::rethrow_exception(failure);
    }

    // Write pax end-of-archive marker (two 512-byte blocks of zeros)
    // so that tools like bsdtar / GNU tar can detect the archive end.
    {
        std::array<std::byte, 1024> eoa{};
        callbacks.write_chunk(PaxChunk{.slice = 0, .bytes = std::span(eoa)});
        blake3_hasher_update(&hasher, eoa.data(), eoa.size());
        stats.output_bytes.fetch_add(eoa.size(), std::memory_order_relaxed);
    }

    std::array<uint8_t, BLAKE3_OUT_LEN> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());
    string hex;
    for (uint8_t b : hash) {
        hex += format("{:02x}", static_cast<unsigned>(b));
    }
    callbacks.end_slice(0);
    return PaxWriteResult{
        .input_bytes = stats.input_bytes.load(std::memory_order_relaxed),
        .output_bytes = stats.output_bytes.load(std::memory_order_relaxed),
        .walked_entries = stats.walked_entries.load(std::memory_order_relaxed),
        .slices = emitted_slice_count,
        .blake3_hex = hex,
    };
}

} // namespace

PaxWriteResult write_pax(const PaxWriterOptions &opts,
                         PaxWriterCallbacks callbacks) {
    return write_pax_archive(opts, std::move(callbacks));
}

PaxLocalOutputResult
write_pax_to_local_output(const PaxWriterOptions &writer_opts,
                          const PaxLocalOutputOptions &out_opts) {
    PaxOutputSession output(out_opts);
    auto callbacks = output.callbacks();
    PaxWriteResult result = write_pax(writer_opts, std::move(callbacks));
    output.finish();
    return PaxLocalOutputResult{std::move(result), output.output_target()};
}

} // namespace neotape
