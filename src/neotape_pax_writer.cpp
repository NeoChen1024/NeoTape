#include "neotape/bounded_buffer.hpp"
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
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
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
constexpr size_t DEFAULT_OUTPUT_BUFFER_SIZE = 64UL * 1024 * 1024;
constexpr size_t BB0_CAPACITY_ENTRIES = 64UL * 1024;
constexpr size_t COMPLETED_QUEUE_MULTIPLIER = 2;
constexpr size_t PAX_ENTRY_OVERHEAD_RESERVE = 16UL * 1024;
constexpr size_t STREAM_FLUSH_THRESH = 4UL * 1024 * 1024;

// ========================== Types ==========================

using Options = PaxWriterOptions;

struct ArchiveStats {
    std::atomic<uint64_t> input_bytes{0};
    std::atomic<uint64_t> output_bytes{0};
    std::atomic<uint64_t> walked_entries{0};
    std::atomic<bool> done{false};
};

struct Result {
    uint64_t seq;
    std::vector<std::byte> bytes; // empty is a valid skipped/warned entry
};

struct WorkItem {
    uint64_t seq;
    archive_entry *entry;
    int fd; // -1 = no data
};

struct PlannedEntry {
    uint64_t slice = 0;
    uint64_t file_num = 0;
    char kind = '?';
    uint64_t size = 0;
    string path;
};

struct PlanRecord {
    std::optional<string> chdir_dir;
    std::optional<PlannedEntry> entry;
};

enum class SlotState : uint8_t { IDLE, BUSY };

// ── BBSink: streaming accumulator to BoundedBuffer ──

struct BBSink {
    BoundedBuffer *dest;
    ArchiveStats *stats;
    std::vector<std::byte> accum;
    bool drop_mode = false;
};

la_ssize_t bb_sink_write(archive *, void *client, const void *data,
                         size_t len) {
    auto *sink = static_cast<BBSink *>(client);
    if (sink->drop_mode)
        return static_cast<la_ssize_t>(len);

    auto *bytes = static_cast<const std::byte *>(data);
    sink->accum.insert(sink->accum.end(), bytes, bytes + len);
    if (sink->accum.size() >= STREAM_FLUSH_THRESH) {
        size_t chunk_size = sink->accum.size();
        if (!sink->dest->push(std::move(sink->accum)))
            return -1;
        sink->stats->input_bytes.fetch_add(chunk_size,
                                           std::memory_order_relaxed);
        sink->accum = {};
        sink->accum.reserve(STREAM_FLUSH_THRESH);
    }
    return static_cast<la_ssize_t>(len);
}

int bb_sink_close(archive *, void *) { return ARCHIVE_OK; }

string stat_rate(uint64_t bytes_per_second) {
    return neotape::humanize_number(static_cast<size_t>(bytes_per_second));
}

string stat_count_rate(uint64_t items_per_second) {
    if (items_per_second < 1000)
        return format("{}", items_per_second);
    if (items_per_second < 1000UL * 1000)
        return format("{:.1f}k",
                      static_cast<double>(items_per_second) / 1000.0);
    return format("{:.1f}M",
                  static_cast<double>(items_per_second) / (1000.0 * 1000.0));
}

// ====================== Diagnostics ==========================

[[noreturn]] void fail_archive(const char *context, archive *a) {
    const char *msg = archive_error_string(a);
    cerr << format("pax: {}{}\n", context,
                   msg != nullptr ? format(": {}", msg) : string());
    std::exit(1);
}

[[noreturn]] void fail_errno(const string &context) {
    cerr << format("pax: {}: {}\n", context, std::strerror(errno));
    std::exit(1);
}

void check_archive(int r, archive *a, const char *context) {
    if (r == ARCHIVE_OK)
        return;
    if (r == ARCHIVE_WARN) {
        const char *msg = archive_error_string(a);
        cerr << format("pax: warning: {}{}\n", context,
                       msg != nullptr ? format(": {}", msg) : string());
        return;
    }
    fail_archive(context, a);
}

void warn_archive(const char *context, archive *a) {
    const char *msg = archive_error_string(a);
    cerr << format("pax: warning: {}{}\n", context,
                   msg != nullptr ? format(": {}", msg) : string());
}

bool locale_name_is_utf8(const char *name) {
    if (name == nullptr)
        return false;
    string locale_name(name);
    std::ranges::transform(
        locale_name, locale_name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return locale_name.find("utf-8") != string::npos ||
           locale_name.find("utf8") != string::npos;
}

void ensure_utf8_ctype_locale_impl() {
    const char *locale_name = std::setlocale(LC_CTYPE, "");
    if (locale_name_is_utf8(locale_name))
        return;
    for (const char *fb : {"C.UTF-8", "en_US.UTF-8"}) {
        locale_name = std::setlocale(LC_CTYPE, fb);
        if (locale_name_is_utf8(locale_name))
            return;
    }
}

uint64_t parse_u64_field(const string &field, const fs::path &path,
                         uint64_t record_num) {
    char *end = nullptr;
    unsigned long long value = std::strtoull(field.c_str(), &end, 10);
    if (end == field.c_str() || *end != '\0')
        throw std::runtime_error(
            format("{}:{}: invalid numeric field", path.string(), record_num));
    return static_cast<uint64_t>(value);
}

PlanRecord parse_plan_record(string_view text, const fs::path &path,
                             uint64_t record_num) {
    if (text.rfind("/chdir/", 0) == 0)
        return PlanRecord{
            .chdir_dir = string(text.substr(7)),
            .entry = std::nullopt,
        };
    if (text.empty() || text.front() != '/')
        throw std::runtime_error(
            format("{}:{}: invalid plan record", path.string(), record_num));

    vector<string> fields;
    size_t start = 1;
    for (size_t i = 1; i <= text.size() && fields.size() < 4; ++i) {
        if (i == text.size() || text[i] == '/') {
            fields.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (fields.size() != 4 || start > text.size())
        throw std::runtime_error(
            format("{}:{}: invalid entry record", path.string(), record_num));

    string entry_path(text.substr(start));
    if (entry_path.empty() || fields[2].size() != 1)
        throw std::runtime_error(
            format("{}:{}: invalid entry record", path.string(), record_num));

    return PlanRecord{
        .chdir_dir = std::nullopt,
        .entry =
            PlannedEntry{
                .slice = parse_u64_field(fields[0], path, record_num),
                .file_num = parse_u64_field(fields[1], path, record_num),
                .kind = fields[2][0],
                .size = parse_u64_field(fields[3], path, record_num),
                .path = std::move(entry_path),
            },
    };
}

vector<PlanRecord> read_plan_records(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error(
            format("open {}: {}", path.string(), std::strerror(errno)));
    string data((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());

    vector<PlanRecord> records;
    uint64_t record_num = 1;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find("\0\n", pos, 2);
        if (end == string::npos)
            throw std::runtime_error(format("{}:{}: unterminated plan record",
                                            path.string(), record_num));
        records.push_back(parse_plan_record(
            string_view(data.data() + pos, end - pos), path, record_num));
        pos = end + 2;
        ++record_num;
    }
    return records;
}

// ====================== Entry Formatting =====================

void mark_link_target_as_utf8(archive_entry *entry) {
    if (const char *s = archive_entry_symlink(entry); s != nullptr)
        archive_entry_update_symlink_utf8(entry, s);
    else if (const char *h = archive_entry_hardlink(entry); h != nullptr)
        archive_entry_update_hardlink_utf8(entry, h);
}

string entry_owner_name(archive_entry *entry) {
    if (const char *n = archive_entry_uname(entry); n != nullptr)
        return n;
    return std::to_string(archive_entry_uid(entry));
}

string entry_group_name(archive_entry *entry) {
    if (const char *n = archive_entry_gname(entry); n != nullptr)
        return n;
    return std::to_string(archive_entry_gid(entry));
}

string entry_timestamp(archive_entry *entry) {
    std::time_t t = archive_entry_mtime(entry);
    std::tm lt{};
    if (localtime_r(&t, &lt) == nullptr)
        return "00000000T000000+0000";
    char buf[32]{};
    if (std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%S%z", &lt) == 0)
        return "00000000T000000+0000";
    return buf;
}

string entry_display_path(archive_entry *entry) {
    string p = archive_entry_pathname(entry) != nullptr
                   ? archive_entry_pathname(entry)
                   : "";
    if (archive_entry_filetype(entry) == AE_IFDIR && !p.empty() &&
        p.back() != '/')
        p += '/';
    return p;
}

string entry_size_display(archive_entry *entry) {
    la_int64_t sz = archive_entry_size(entry);
    if (sz < 0)
        return "?";
    return neotape::humanize_number(static_cast<size_t>(sz));
}

string verbose_line(archive_entry *entry) {
    string mode = archive_entry_strmode(entry);
    if (mode.size() > 10)
        mode.resize(10);
    if (archive_entry_hardlink(entry) != nullptr && !mode.empty())
        mode[0] = 'h';
    return format("{} {:3} {:>10} {:>10} {:>6} [{}] {}", mode,
                  archive_entry_nlink(entry), entry_owner_name(entry),
                  entry_group_name(entry), entry_size_display(entry),
                  entry_timestamp(entry), entry_display_path(entry));
}

int open_entry_file(archive_entry *entry) {
    const char *src = archive_entry_sourcepath(entry);
    if (src == nullptr)
        src = archive_entry_pathname(entry);
    if (src == nullptr)
        return -1;
    int fd = open(src, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        cerr << format("pax: warning: open {}: {}\n", src,
                       std::strerror(errno));
    return fd;
}

archive_entry *planned_entry_from_path(archive *disk, const string &path) {
    archive_entry *entry = archive_entry_new();
    if (!entry)
        throw std::runtime_error("cannot allocate entry");
    archive_entry_set_pathname_utf8(entry, path.c_str());
    archive_entry_copy_sourcepath(entry, path.c_str());
    int r = archive_read_disk_entry_from_file(disk, entry, -1, nullptr);
    if (r == ARCHIVE_FATAL)
        fail_archive("read filesystem", disk);
    if (r < ARCHIVE_OK)
        warn_archive("read filesystem", disk);
    archive_entry_set_pathname_utf8(entry, path.c_str());
    archive_entry_copy_sourcepath(entry, path.c_str());
    mark_link_target_as_utf8(entry);
    return entry;
}

// ====================== Data Copying =========================

void copy_file_data(archive *writer, archive_entry *entry, int fd) {
    const char *src = archive_entry_sourcepath(entry);
    if (src == nullptr)
        src = archive_entry_pathname(entry);
    if (src == nullptr)
        fail_archive("entry has no source path", writer);

    thread_local vector<char> buf;
    if (buf.empty())
        buf.resize(SMALL_FILE_THRESHOLD);
    for (;;) {
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0)
            fail_errno(string("read ") + src);
        if (n == 0)
            break;
        ssize_t w =
            archive_write_data(writer, buf.data(), static_cast<size_t>(n));
        if (w < 0)
            fail_archive("write file data", writer);
        if (w != n) {
            cerr << format("pax: short archive write for {}\n", src);
            std::exit(1);
        }
    }
}

// ====================== Drop-Mode Writer Helpers ============

struct BufCtx {
    vector<std::byte> buf;
    bool drop = false;
};

int drop_open(archive *, void *) { return ARCHIVE_OK; }

la_ssize_t drop_write(archive *, void *client, const void *data, size_t len) {
    auto *ctx = static_cast<BufCtx *>(client);
    if (ctx->drop)
        return static_cast<la_ssize_t>(len);
    auto *bytes = static_cast<const std::byte *>(data);
    ctx->buf.insert(ctx->buf.end(), bytes, bytes + len);
    return static_cast<la_ssize_t>(len);
}

int drop_close(archive *, void *) { return ARCHIVE_OK; }

vector<std::byte> serialize_entry(archive_entry *entry, int fd) {
    archive *a = archive_write_new();
    if (!a) {
        cerr << "pax: cannot allocate archive writer\n";
        std::exit(1);
    }
    check_archive(archive_write_add_filter_none(a), a, "set uncompressed");
    check_archive(archive_write_set_format_pax(a), a, "set pax format");
    check_archive(
        archive_write_set_options(a, "xattrheader=ALL,hdrcharset=UTF-8"), a,
        "set options");
    check_archive(archive_write_set_bytes_per_block(a, 512), a,
                  "set block size");
    check_archive(archive_write_set_bytes_in_last_block(a, 1), a,
                  "set last block");

    BufCtx ctx;
    la_int64_t entry_size = archive_entry_size(entry);
    if (fd >= 0 && entry_size > 0) {
        size_t reserve_size =
            static_cast<size_t>(entry_size) + PAX_ENTRY_OVERHEAD_RESERVE;
        ctx.buf.reserve(reserve_size);
    } else {
        ctx.buf.reserve(PAX_ENTRY_OVERHEAD_RESERVE);
    }
    check_archive(
        archive_write_open(a, &ctx, drop_open, drop_write, drop_close), a,
        "open per-entry writer");

    int r = archive_write_header(a, entry);
    if (r == ARCHIVE_FATAL)
        fail_archive("write header", a);
    if (r < ARCHIVE_OK) {
        warn_archive("write header", a);
        ctx.drop = true;
        archive_write_close(a);
        archive_write_free(a);
        return {};
    }
    if (fd >= 0)
        copy_file_data(a, entry, fd);
    check_archive(archive_write_finish_entry(a), a, "finish entry");
    ctx.drop = true;
    check_archive(archive_write_close(a), a, "close writer");
    archive_write_free(a);
    return std::move(ctx.buf);
}

// ====================== Streaming to BB1 ====================

void stream_large_entry(BBSink &sink, archive_entry *entry, int fd) {
    sink.drop_mode = false;
    archive *a = archive_write_new();
    if (!a) {
        cerr << "pax: cannot allocate archive writer\n";
        std::exit(1);
    }
    check_archive(archive_write_add_filter_none(a), a, "set uncompressed");
    check_archive(archive_write_set_format_pax(a), a, "set pax format");
    check_archive(
        archive_write_set_options(a, "xattrheader=ALL,hdrcharset=UTF-8"), a,
        "set options");
    check_archive(archive_write_set_bytes_per_block(a, 512), a,
                  "set block size");
    check_archive(archive_write_set_bytes_in_last_block(a, 1), a,
                  "set last block");
    check_archive(
        archive_write_open(a, &sink, drop_open, bb_sink_write, bb_sink_close),
        a, "open streaming writer");

    int r = archive_write_header(a, entry);
    if (r == ARCHIVE_FATAL)
        fail_archive("write header", a);
    if (r < ARCHIVE_OK) {
        warn_archive("write header", a);
        sink.drop_mode = true;
        archive_write_close(a);
        archive_write_free(a);
        return;
    }
    copy_file_data(a, entry, fd);
    check_archive(archive_write_finish_entry(a), a, "finish entry");

    if (!sink.accum.empty()) {
        size_t chunk_size = sink.accum.size();
        sink.dest->push(std::move(sink.accum));
        sink.stats->input_bytes.fetch_add(chunk_size,
                                          std::memory_order_relaxed);
    }

    sink.drop_mode = true;
    archive_write_close(a);
    archive_write_free(a);
}

// ====================== BlockingQueue ========================

template <typename T> class BlockingQueue {
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    size_t max_size_ = 0;
    bool closed_ = false;

  public:
    BlockingQueue() = default;
    explicit BlockingQueue(size_t max_size) : max_size_(max_size) {}

    void push(T item) {
        std::unique_lock l(mtx_);
        if (max_size_ > 0)
            cv_.wait(l,
                     [this] { return queue_.size() < max_size_ || closed_; });
        if (closed_)
            return;
        queue_.push(std::move(item));
        cv_.notify_one();
    }
    std::optional<T> pop() {
        std::unique_lock l(mtx_);
        cv_.wait(l, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty())
            return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        cv_.notify_one();
        return item;
    }
    std::optional<T> try_pop() {
        std::lock_guard l(mtx_);
        if (queue_.empty())
            return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        cv_.notify_one();
        return item;
    }
    void close() {
        std::lock_guard l(mtx_);
        closed_ = true;
        cv_.notify_all();
    }
};

// ====================== Worker Slot ==========================

struct WorkerSlot {
    std::mutex mtx;
    std::condition_variable cv;
    SlotState state = SlotState::IDLE;
    WorkItem work;
};

// ====================== Worker Main ==========================

void worker_main(size_t slot_idx, WorkerSlot &slot,
                 BlockingQueue<Result> &completed_queue,
                 BlockingQueue<size_t> &idle_queue, std::mutex &notify_mtx,
                 std::condition_variable &notify_cv,
                 uint64_t &notify_generation, std::atomic<bool> &done) {
    for (;;) {
        WorkItem w;
        {
            std::unique_lock l(slot.mtx);
            slot.cv.wait(l, [&] {
                return slot.state == SlotState::BUSY || done.load();
            });
            if (done.load() && slot.state != SlotState::BUSY)
                return;
            w = std::move(slot.work);
        }

        auto bytes = serialize_entry(w.entry, w.fd);
        archive_entry_free(w.entry);
        if (w.fd >= 0)
            close(w.fd);

        completed_queue.push(Result{w.seq, std::move(bytes)});
        {
            std::lock_guard l(slot.mtx);
            slot.state = SlotState::IDLE;
        }
        idle_queue.push(slot_idx);
        {
            std::lock_guard l(notify_mtx);
            ++notify_generation;
        }
        notify_cv.notify_one();
    }
}

// ====================== Serializer Main ======================

void serializer_main(vector<WorkerSlot> &slots, BBSink &bb1_sink,
                     BlockingQueue<Result> &bb0,
                     BlockingQueue<Result> &completed_queue,
                     ArchiveStats &stats, std::mutex &notify_mtx,
                     std::condition_variable &notify_cv,
                     uint64_t &notify_generation, std::atomic<bool> &done) {
    uint64_t expected = 0;
    size_t nworkers = slots.size() - 1;
    std::map<uint64_t, Result> pending;

    auto notify_large_slot_idle = [&] {
        slots[nworkers].cv.notify_all();
        {
            std::lock_guard l(notify_mtx);
            ++notify_generation;
        }
        notify_cv.notify_one();
    };

    auto collect_ready_results = [&] {
        bool progress = false;
        while (auto r = bb0.try_pop()) {
            pending.emplace(r->seq, std::move(*r));
            progress = true;
        }
        while (auto r = completed_queue.try_pop()) {
            pending.emplace(r->seq, std::move(*r));
            progress = true;
        }
        return progress;
    };

    auto emit_pending = [&] {
        bool progress = false;
        for (;;) {
            auto it = pending.find(expected);
            if (it == pending.end())
                break;
            Result r = std::move(it->second);
            pending.erase(it);
            if (!r.bytes.empty()) {
                size_t chunk_size = r.bytes.size();
                if (bb1_sink.dest->push(std::move(r.bytes)))
                    stats.input_bytes.fetch_add(chunk_size,
                                                std::memory_order_relaxed);
            }
            expected++;
            progress = true;
        }
        return progress;
    };

    auto stream_expected_large = [&] {
        WorkItem w{};
        bool have_large = false;
        {
            std::lock_guard l(slots[nworkers].mtx);
            if (slots[nworkers].state == SlotState::BUSY &&
                slots[nworkers].work.seq == expected) {
                w = slots[nworkers].work;
                have_large = true;
            }
        }
        if (!have_large)
            return false;

        stream_large_entry(bb1_sink, w.entry, w.fd);
        archive_entry_free(w.entry);
        if (w.fd >= 0)
            close(w.fd);
        {
            std::lock_guard l(slots[nworkers].mtx);
            slots[nworkers].state = SlotState::IDLE;
        }
        notify_large_slot_idle();
        expected++;
        return true;
    };

    auto any_busy_slot = [&] {
        for (WorkerSlot &slot : slots) {
            std::lock_guard l(slot.mtx);
            if (slot.state == SlotState::BUSY)
                return true;
        }
        return false;
    };

    uint64_t seen_generation = 0;
    for (;;) {
        bool progress = false;
        progress = collect_ready_results() || progress;

        for (;;) {
            bool ordered_progress = false;
            ordered_progress = emit_pending() || ordered_progress;
            ordered_progress = stream_expected_large() || ordered_progress;
            if (!ordered_progress)
                break;
            progress = true;
            collect_ready_results();
        }

        if (done.load() && pending.empty() && !any_busy_slot()) {
            if (!collect_ready_results() && pending.empty())
                break;
            continue;
        }

        if (progress)
            continue;

        std::unique_lock l(notify_mtx);
        seen_generation = notify_generation;
        notify_cv.wait(l, [&] { return notify_generation != seen_generation; });
        seen_generation = notify_generation;
    }
}

// ====================== Archive Emission =====================

PaxWriteResult write_planned_pax_archive(const Options &opts,
                                         PaxWriterCallbacks callbacks) {
    if (!callbacks.begin_slice)
        callbacks.begin_slice = [](uint64_t) {};
    if (!callbacks.write_chunk)
        callbacks.write_chunk = [](PaxChunk) {};
    if (!callbacks.end_slice)
        callbacks.end_slice = [](uint64_t) {};

    if (!opts.plan_path.has_value())
        throw std::runtime_error("planned pax archive requires a plan path");
    if (opts.chdir_dir.has_value() && chdir(opts.chdir_dir->c_str()) != 0)
        fail_errno(string("chdir ") + *opts.chdir_dir);

    vector<PlanRecord> records = read_plan_records(*opts.plan_path);
    ArchiveStats stats;
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    archive *disk = archive_read_disk_new();
    if (!disk)
        throw std::runtime_error("cannot allocate disk reader");
    check_archive(archive_read_disk_set_symlink_physical(disk), disk,
                  "set physical symlink");
    check_archive(archive_read_disk_set_standard_lookup(disk), disk,
                  "set uid/gid name lookup");

    archive_entry_linkresolver *resolver = archive_entry_linkresolver_new();
    if (!resolver)
        throw std::runtime_error("cannot allocate hardlink resolver");
    archive *tmp = archive_write_new();
    archive_write_add_filter_none(tmp);
    archive_write_set_format_pax(tmp);
    archive_entry_linkresolver_set_strategy(resolver, archive_format(tmp));
    archive_write_free(tmp);

    std::optional<uint64_t> current_slice;
    uint64_t emitted_slices = 0;

    auto emit_entry = [&](archive_entry *entry, uint64_t slice) {
        if (!entry)
            return;
        stats.walked_entries.fetch_add(1, std::memory_order_relaxed);

        if (opts.verbose > 1)
            cerr << format("\n{}", verbose_line(entry));
        else if (opts.verbose > 0)
            cerr << format("\na {}", entry_display_path(entry));

        bool is_reg = (archive_entry_filetype(entry) == AE_IFREG);
        la_int64_t size = archive_entry_size(entry);
        bool has_data = (is_reg && size > 0);
        int fd = has_data ? open_entry_file(entry) : -1;
        if (has_data && fd < 0) {
            archive_entry_free(entry);
            return;
        }

        vector<std::byte> bytes = serialize_entry(entry, fd);
        archive_entry_free(entry);
        if (fd >= 0)
            close(fd);
        if (bytes.empty())
            return;

        callbacks.write_chunk(PaxChunk{
            .slice = slice,
            .bytes = std::span<const std::byte>(bytes.data(), bytes.size()),
        });
        blake3_hasher_update(&hasher, bytes.data(), bytes.size());
        stats.input_bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
        stats.output_bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
    };

    for (const PlanRecord &record : records) {
        if (record.chdir_dir.has_value()) {
            if (chdir(record.chdir_dir->c_str()) != 0)
                fail_errno(string("chdir ") + *record.chdir_dir);
            continue;
        }
        if (!record.entry.has_value())
            continue;

        const PlannedEntry &planned = *record.entry;
        if (!current_slice.has_value() || *current_slice != planned.slice) {
            if (current_slice.has_value())
                callbacks.end_slice(*current_slice);
            current_slice = planned.slice;
            callbacks.begin_slice(*current_slice);
            ++emitted_slices;
        }

        archive_entry *entry = planned_entry_from_path(disk, planned.path);
        archive_entry *spare = nullptr;
        archive_entry_linkify(resolver, &entry, &spare);
        emit_entry(entry, planned.slice);
        emit_entry(spare, planned.slice);
    }

    for (;;) {
        archive_entry *entry = nullptr;
        archive_entry *spare = nullptr;
        archive_entry_linkify(resolver, &entry, &spare);
        if (!entry && !spare)
            break;
        uint64_t slice = current_slice.value_or(0);
        emit_entry(entry, slice);
        emit_entry(spare, slice);
    }

    if (current_slice.has_value())
        callbacks.end_slice(*current_slice);

    archive_entry_linkresolver_free(resolver);
    archive_read_free(disk);

    std::array<uint8_t, BLAKE3_OUT_LEN> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());
    string hex;
    for (uint8_t b : hash)
        hex += format("{:02x}", static_cast<unsigned>(b));
    return PaxWriteResult{
        .input_bytes = stats.input_bytes.load(std::memory_order_relaxed),
        .output_bytes = stats.output_bytes.load(std::memory_order_relaxed),
        .walked_entries = stats.walked_entries.load(std::memory_order_relaxed),
        .slices = emitted_slices,
        .blake3_hex = hex,
    };
}

PaxWriteResult write_pax_archive(const Options &opts,
                                 PaxWriterCallbacks callbacks) {
    if (opts.plan_path.has_value())
        return write_planned_pax_archive(opts, std::move(callbacks));

    if (!callbacks.begin_slice)
        callbacks.begin_slice = [](uint64_t) {};
    if (!callbacks.write_chunk)
        callbacks.write_chunk = [](PaxChunk) {};
    if (!callbacks.end_slice)
        callbacks.end_slice = [](uint64_t) {};
    callbacks.begin_slice(0);
    uint64_t emitted_slice_count = 1;

    // ── Bounded buffers ──
    BoundedBuffer bb1(opts.output_buf_size);
    BlockingQueue<Result> bb0(BB0_CAPACITY_ENTRIES);
    ArchiveStats stats;

    // ── Output thread ──
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::atomic<bool> output_error{false};
    size_t output_restart_bytes =
        opts.output_buf_size * opts.buffer_percent / 100;

    std::thread stats_thread([&] {
        using clock = std::chrono::steady_clock;
        uint64_t last_in = 0;
        uint64_t last_out = 0;
        uint64_t last_files = 0;
        auto last_time = clock::now();

        while (!stats.done.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stats.done.load(std::memory_order_relaxed))
                break;
            auto now = clock::now();
            double seconds =
                std::chrono::duration<double>(now - last_time).count();
            if (seconds <= 0.0)
                seconds = 1.0;

            uint64_t current_in =
                stats.input_bytes.load(std::memory_order_relaxed);
            uint64_t current_out =
                stats.output_bytes.load(std::memory_order_relaxed);
            uint64_t current_files =
                stats.walked_entries.load(std::memory_order_relaxed);
            uint64_t in_rate =
                static_cast<uint64_t>((current_in - last_in) / seconds);
            uint64_t out_rate =
                static_cast<uint64_t>((current_out - last_out) / seconds);
            uint64_t file_rate =
                static_cast<uint64_t>((current_files - last_files) / seconds);
            size_t buffered = bb1.size_bytes();
            size_t capacity = bb1.capacity_bytes();
            size_t percent =
                capacity == 0
                    ? 0
                    : std::min<size_t>(100, buffered * 100 / capacity);

            // carridge return to overwrite the previous line, but only if we're
            // not on the first line
            cerr << format(
                "\rin @ {:>6}/s, out @ {:>6}/s, files @ {:>6}/s, "
                "{:>6} total, buffer {:3}% full  ",
                stat_rate(in_rate), stat_rate(out_rate),
                stat_count_rate(file_rate),
                neotape::humanize_number(static_cast<size_t>(current_out)),
                percent);

            last_in = current_in;
            last_out = current_out;
            last_files = current_files;
            last_time = now;
        }
    });

    std::thread output_thread([&] {
        bool wait_for_waterline = output_restart_bytes > 0;
        for (;;) {
            auto chunk = wait_for_waterline
                             ? bb1.pop_after_fill(output_restart_bytes)
                             : bb1.pop();
            if (chunk.empty())
                break;
            callbacks.write_chunk(PaxChunk{
                .slice = 0,
                .bytes = std::span<const std::byte>(chunk.data(), chunk.size()),
            });
            blake3_hasher_update(&hasher, chunk.data(), chunk.size());
            stats.output_bytes.fetch_add(chunk.size(),
                                         std::memory_order_relaxed);
            wait_for_waterline =
                output_restart_bytes > 0 && bb1.size_bytes() == 0;
        }
    });

    // ── Worker slots ──
    unsigned nworkers = opts.io_thread > 0 ? opts.io_thread - 1 : 0;
    vector<WorkerSlot> slots(nworkers + 1); // last slot = large
    size_t result_slots =
        std::max<size_t>(1, COMPLETED_QUEUE_MULTIPLIER * nworkers);
    BlockingQueue<Result> completed_queue(result_slots);

    // ── Idle queue (worker slots only) ──
    BlockingQueue<size_t> idle_queue;
    for (size_t i = 0; i < nworkers; i++)
        idle_queue.push(i);

    // ── Shared state ──
    std::atomic<bool> done{false};
    std::mutex notify_mtx;
    std::condition_variable notify_cv;
    uint64_t notify_generation = 0;

    // Streaming sink for serializer BB1 output
    BBSink bb1_sink{&bb1, &stats, {}, false};

    // ── Start workers ──
    vector<std::thread> worker_threads;
    for (size_t i = 0; i < nworkers; i++)
        worker_threads.emplace_back(
            worker_main, i, std::ref(slots[i]), std::ref(completed_queue),
            std::ref(idle_queue), std::ref(notify_mtx), std::ref(notify_cv),
            std::ref(notify_generation), std::ref(done));

    // ── Start serializer ──
    std::thread serializer_thread(
        serializer_main, std::ref(slots), std::ref(bb1_sink), std::ref(bb0),
        std::ref(completed_queue), std::ref(stats), std::ref(notify_mtx),
        std::ref(notify_cv), std::ref(notify_generation), std::ref(done));

    // ── Chdir ──
    if (opts.chdir_dir.has_value() && chdir(opts.chdir_dir->c_str()) != 0)
        fail_errno(string("chdir ") + *opts.chdir_dir);

    // ── Hardlink resolver ──
    archive_entry_linkresolver *resolver = archive_entry_linkresolver_new();
    if (!resolver) {
        cerr << "pax: cannot allocate hardlink resolver\n";
        std::exit(1);
    }
    archive *tmp = archive_write_new();
    archive_write_add_filter_none(tmp);
    archive_write_set_format_pax(tmp);
    archive_entry_linkresolver_set_strategy(resolver, archive_format(tmp));
    archive_write_free(tmp);

    // ── Dispatch lambda ──
    uint64_t next_seq = 0;

    auto notify = [&] {
        std::lock_guard l(notify_mtx);
        ++notify_generation;
        notify_cv.notify_one();
    };

    auto assign_small = [&](uint64_t seq, archive_entry *entry, int fd) {
        size_t idx = idle_queue.pop().value();
        {
            std::lock_guard l(slots[idx].mtx);
            slots[idx].work = WorkItem{seq, entry, fd};
            slots[idx].state = SlotState::BUSY;
        }
        slots[idx].cv.notify_one();
    };

    auto assign_large = [&](uint64_t seq, archive_entry *entry, int fd) {
        size_t idx = nworkers;
        {
            std::unique_lock l(slots[idx].mtx);
            slots[idx].cv.wait(
                l, [&] { return slots[idx].state == SlotState::IDLE; });
            slots[idx].work = WorkItem{seq, entry, fd};
            slots[idx].state = SlotState::BUSY;
        }
        notify();
    };

    auto dispatch_entry = [&](archive_entry *entry) {
        if (!entry)
            return;
        stats.walked_entries.fetch_add(1, std::memory_order_relaxed);

        bool is_reg = (archive_entry_filetype(entry) == AE_IFREG);
        la_int64_t size = archive_entry_size(entry);
        bool has_data = (is_reg && size > 0);

        // new line before each entry, so it'll cooperate with the stats line
        // (which doesn't print a newline)
        if (opts.verbose > 1)
            cerr << format("\n{}", verbose_line(entry));
        else if (opts.verbose > 0)
            cerr << format("\na {}", entry_display_path(entry));

        if (!has_data) {
            uint64_t seq = next_seq++;
            auto bytes = serialize_entry(entry, -1);
            archive_entry_free(entry);
            bb0.push(Result{seq, std::move(bytes)});
            notify();
            return;
        }

        int fd = open_entry_file(entry);
        if (fd < 0) {
            archive_entry_free(entry);
            return;
        }

        uint64_t seq = next_seq++;
        if (nworkers == 0 || static_cast<size_t>(size) > SMALL_FILE_THRESHOLD)
            assign_large(seq, entry, fd);
        else
            assign_small(seq, entry, fd);
    };

    // ─── Walk sources ───
    for (const string &source_arg : opts.sources) {
        neotape::SourceSpec spec;
        try {
            spec = neotape::make_source_spec(source_arg);
        } catch (const std::exception &e) {
            cerr << format("pax: {}\n", e.what());
            std::exit(1);
        }

        archive *disk = archive_read_disk_new();
        if (!disk) {
            cerr << "pax: cannot allocate disk reader\n";
            std::exit(1);
        }
        check_archive(archive_read_disk_set_symlink_physical(disk), disk,
                      "set physical symlink");
        if (opts.one_file_system)
            check_archive(archive_read_disk_set_behavior(
                              disk, ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS),
                          disk, "set one-file-system");
        check_archive(archive_read_disk_set_standard_lookup(disk), disk,
                      "set uid/gid name lookup");
        check_archive(archive_read_disk_open(disk, spec.open_path.c_str()),
                      disk, "open source path");

        for (;;) {
            archive_entry *entry = archive_entry_new();
            if (!entry) {
                cerr << "pax: cannot allocate entry\n";
                std::exit(1);
            }

            int r = archive_read_next_header2(disk, entry);
            if (r == ARCHIVE_EOF) {
                archive_entry_free(entry);
                break;
            }
            if (r == ARCHIVE_FATAL)
                fail_archive("read filesystem", disk);
            if (r < ARCHIVE_OK) {
                warn_archive("read filesystem", disk);
                archive_entry_free(entry);
                continue;
            }

            if (archive_read_disk_can_descend(disk)) {
                r = archive_read_disk_descend(disk);
                if (r == ARCHIVE_FATAL)
                    fail_archive("descend", disk);
                if (r < ARCHIVE_OK)
                    warn_archive("descend", disk);
            }

            const char *src = archive_entry_sourcepath(entry);
            if (src) {
                string ap = neotape::archive_path_for_source(spec, src);
                archive_entry_set_pathname_utf8(entry, ap.c_str());
            }
            mark_link_target_as_utf8(entry);

            archive_entry *spare = nullptr;
            archive_entry_linkify(resolver, &entry, &spare);
            dispatch_entry(entry);
            dispatch_entry(spare);
        }

        archive_read_close(disk);
        archive_read_free(disk);
    }

    // Hardlink flush
    for (;;) {
        archive_entry *entry = nullptr;
        archive_entry *spare = nullptr;
        archive_entry_linkify(resolver, &entry, &spare);
        if (!entry && !spare)
            break;
        dispatch_entry(entry);
        dispatch_entry(spare);
    }

    archive_entry_linkresolver_free(resolver);

    // ── Shutdown ──
    idle_queue.close();
    done.store(true);

    for (auto &s : slots) {
        std::lock_guard l(s.mtx);
        s.cv.notify_all();
    }
    {
        std::lock_guard l(notify_mtx);
        ++notify_generation;
        notify_cv.notify_all();
    }

    for (auto &t : worker_threads)
        if (t.joinable())
            t.join();
    if (serializer_thread.joinable())
        serializer_thread.join();

    bb1.close();
    output_thread.join();
    stats.done.store(true, std::memory_order_relaxed);
    if (stats_thread.joinable())
        stats_thread.join();

    if (output_error.load())
        throw std::runtime_error("write error");

    // ── BLAKE3 ──
    std::array<uint8_t, BLAKE3_OUT_LEN> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());
    string hex;
    for (uint8_t b : hash)
        hex += format("{:02x}", static_cast<unsigned>(b));
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

void ensure_utf8_ctype_locale() { ensure_utf8_ctype_locale_impl(); }

} // namespace neotape
