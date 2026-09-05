#include "neotape/bounded_buffer.hpp"
#include "neotape/closable_queue.hpp"
#include "neotape/common.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/plan.hpp"
#include "neotape/progress.hpp"
#include "neotape/result_store.hpp"

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
};

struct Result {
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
    bool count_input = true;
    vector<std::byte> bytes;
    EntryHandle entry;
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
                .write_chunk = [&](PaxChunk chunk) { write_chunk(chunk); },
                .end_slice =
                    [&](uint64_t) { close_file("close slice output"); },
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
            .write_chunk = [&](PaxChunk chunk) { write_chunk(chunk); },
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

void print_pax_progress(uint64_t in_rate, uint64_t out_rate, uint64_t file_rate,
                        uint64_t current_slice, uint64_t current_out,
                        size_t buffer_percent) {
    write_progress(
        format("in @ {:>6}/s, out @ {:>6}/s, files @ {:>6}/s, "
               "slice {:>6}, {:>6} total, buffer {:3}% full  ",
               stat_rate(in_rate), stat_rate(out_rate), count_rate(file_rate),
               current_slice,
               neotape::humanize_number(static_cast<size_t>(current_out)),
               buffer_percent));
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
        write_diagnostic(
            format("pax: warning: {}{}", context,
                   msg != nullptr ? format(": {}", msg) : string()));
        return;
    }
    throw_archive(context, a);
}

void warn_archive(const char *context, archive *a) {
    const char *msg = archive_error_string(a);
    write_diagnostic(format("pax: warning: {}{}", context,
                            msg != nullptr ? format(": {}", msg) : string()));
}

// ====================== Entry Formatting =====================

void copy_pathname(archive_entry *entry, const string &path) {
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
    return escape_bytes_for_diagnostic(p);
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
        write_diagnostic(format("pax: warning: open {}: {}",
                                escape_bytes_for_diagnostic(src),
                                std::strerror(errno)));
    }
    return fd;
}

archive_entry *planned_entry_from_path(archive *disk, const string &path) {
    EntryHandle entry(archive_entry_new());
    if (!entry) {
        throw std::runtime_error("cannot allocate entry");
    }
    copy_pathname(entry.get(), path);
    archive_entry_copy_sourcepath(entry.get(), path.c_str());
    int const r =
        archive_read_disk_entry_from_file(disk, entry.get(), -1, nullptr);
    if (r == ARCHIVE_FATAL) {
        throw_archive("read filesystem", disk);
    }
    if (r < ARCHIVE_OK) {
        warn_archive("read filesystem", disk);
    }
    copy_pathname(entry.get(), path);
    archive_entry_copy_sourcepath(entry.get(), path.c_str());
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
        archive_write_set_options(a, "xattrheader=ALL,hdrcharset=BINARY"), a,
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
        archive_write_set_options(a, "xattrheader=ALL,hdrcharset=BINARY"), a,
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
                          ResultStore<Result> &results, BoundedBuffer &bb1,
                          PipelineCancel &cancel) {
    for (;;) {
        auto item = work_queue.pop();
        if (!item.has_value()) {
            return;
        }
        try {
            int const fd = open_entry_file(item->entry.get());
            if (fd < 0) {
                if (!results.put(item->seq, Result{{}})) {
                    return;
                }
                continue;
            }
            vector<std::byte> bytes = serialize_entry(item->entry.get(), fd);
            close(fd);
            if (!results.put(item->seq, Result{std::move(bytes)})) {
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

bool emit_bytes_to_bb1(BBSink &sink, vector<std::byte> bytes,
                       bool count_input) {
    if (bytes.empty()) {
        return true;
    }
    size_t chunk_size = bytes.size();
    if (!sink.dest->push(std::move(bytes))) {
        return false;
    }
    if (count_input) {
        sink.stats->input_bytes.fetch_add(chunk_size,
                                          std::memory_order_relaxed);
    }
    return true;
}

void pipeline_serializer_main(ClosableQueue<PipelineWorkItem> &work_queue,
                              ClosableQueue<PipelineOrderItem> &order_queue,
                              ResultStore<Result> &results, BBSink &bb1_sink,
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
                keep_running = emit_bytes_to_bb1(
                    bb1_sink, std::move(item->bytes), item->count_input);
                break;
            case PipelineOrderKind::WorkerResult: {
                auto result = results.take(item->seq);
                if (!result.has_value()) {
                    keep_running = false;
                    break;
                }
                keep_running =
                    emit_bytes_to_bb1(bb1_sink, std::move(result->bytes), true);
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
                ArchiveStats &stats, blake3_hasher &hasher,
                uint64_t output_slice = 0)
        : opts_(opts), callbacks_(callbacks), stats_(stats), hasher_(hasher),
          bb1_(opts.output_buf_size),
          order_queue_(pipeline_order_queue_capacity(opts)),
          work_queue_(pipeline_work_queue_capacity(opts)),
          results_(pipeline_result_capacity(opts)),
          bb1_sink_{&bb1_, &stats_, {}, false}, output_slice_(output_slice) {}

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

    bool enqueue_inline(uint64_t seq, vector<std::byte> bytes,
                        bool count_input = true) {
        PipelineOrderItem item;
        item.kind = PipelineOrderKind::InlineBytes;
        item.seq = seq;
        item.count_input = count_input;
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
                    .slice = output_slice_,
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
    ResultStore<Result> results_;
    PipelineCancel cancel_;
    BBSink bb1_sink_;
    uint64_t output_slice_ = 0;
    vector<std::thread> workers_;
    std::thread serializer_thread_;
    std::thread output_thread_;
    bool started_ = false;
    bool joined_ = false;
};

void dispatch_entry_to_pipeline(const Options &opts, ArchiveStats &stats,
                                PaxPipeline &pipeline, uint64_t &next_seq,
                                EntryHandle entry) {
    if (!entry) {
        return;
    }
    stats.walked_entries.fetch_add(1, std::memory_order_relaxed);

    bool const is_reg = (archive_entry_filetype(entry.get()) == AE_IFREG);
    la_int64_t const size = archive_entry_size(entry.get());
    bool const has_data = (is_reg && size > 0);

    if (opts.verbose > 1) {
        write_diagnostic(verbose_line(entry.get()));
    } else if (opts.verbose > 0) {
        write_diagnostic(format("a {}", entry_display_path(entry.get())));
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
}

class SlicePipeline {
  public:
    SlicePipeline(const Options &opts, PaxWriterCallbacks &callbacks,
                  ArchiveStats &stats, blake3_hasher &hasher)
        : opts_(opts), callbacks_(callbacks), stats_(stats), hasher_(hasher) {}

    ~SlicePipeline() noexcept {
        try {
            abort();
        } catch (...) {
        }
    }

    void begin_slice(uint64_t slice) {
        if (active_slice_.has_value()) {
            throw std::runtime_error("slice output already active");
        }

        callbacks_.begin_slice(slice);
        active_slice_ = slice;
        next_seq_ = 0;
        {
            std::scoped_lock lock(pipeline_mtx_);
            pipeline_ = std::make_unique<PaxPipeline>(opts_, callbacks_, stats_,
                                                      hasher_, slice);
        }
        try {
            pipeline_->start();
        } catch (...) {
            std::scoped_lock lock(pipeline_mtx_);
            pipeline_.reset();
            active_slice_.reset();
            throw;
        }
    }

    void emit_entry(EntryHandle entry) {
        if (!active_slice_.has_value()) {
            throw std::runtime_error("slice pipeline is not active");
        }
        if (pipeline_ == nullptr) {
            throw std::runtime_error("slice pipeline is not available");
        }
        dispatch_entry_to_pipeline(opts_, stats_, *pipeline_, next_seq_,
                                   std::move(entry));
    }

    void emit_eoa() {
        if (pipeline_ == nullptr) {
            throw std::runtime_error("slice pipeline is not available");
        }
        if (!pipeline_->enqueue_inline(
                next_seq_++, vector<std::byte>(1024, std::byte{0}), false)) {
            pipeline_->rethrow_if_failed();
            throw std::runtime_error(
                "pax pipeline closed while enqueueing end-of-archive marker");
        }
    }

    void finish_slice() {
        if (!active_slice_.has_value()) {
            return;
        }
        if (pipeline_ == nullptr) {
            throw std::runtime_error("slice pipeline is not available");
        }
        pipeline_->finish_input();
        pipeline_->join();

        uint64_t const slice = *active_slice_;
        {
            std::scoped_lock lock(pipeline_mtx_);
            pipeline_.reset();
        }
        active_slice_.reset();
        next_seq_ = 0;
        callbacks_.end_slice(slice);
    }

    void abort() {
        if (pipeline_ != nullptr) {
            pipeline_->request_cancel(std::make_exception_ptr(
                std::runtime_error("slice pipeline aborted")));
            try {
                pipeline_->join();
            } catch (...) {
            }
        }
        {
            std::scoped_lock lock(pipeline_mtx_);
            pipeline_.reset();
        }
        active_slice_.reset();
        next_seq_ = 0;
    }

    size_t buffered_bytes() const {
        std::scoped_lock lock(pipeline_mtx_);
        if (pipeline_ == nullptr) {
            return 0;
        }
        return pipeline_->buffered_bytes();
    }

    size_t buffer_capacity() const {
        std::scoped_lock lock(pipeline_mtx_);
        if (pipeline_ == nullptr) {
            return 0;
        }
        return pipeline_->buffer_capacity();
    }

  private:
    const Options &opts_;
    PaxWriterCallbacks &callbacks_;
    ArchiveStats &stats_;
    blake3_hasher &hasher_;
    std::optional<uint64_t> active_slice_;
    // Protect lifetime while the stats thread samples the active buffer.
    mutable std::mutex pipeline_mtx_;
    std::unique_ptr<PaxPipeline> pipeline_;
    uint64_t next_seq_ = 0;
};

// ====================== Archive Emission =====================

using DiskHandle = std::unique_ptr<archive, decltype(&archive_read_free)>;
using ResolverHandle =
    std::unique_ptr<archive_entry_linkresolver,
                    decltype(&archive_entry_linkresolver_free)>;

DiskHandle open_disk_reader(bool one_file_system) {
    DiskHandle disk(archive_read_disk_new(), archive_read_free);
    if (!disk)
        throw std::runtime_error("cannot allocate disk reader");
    check_archive_throw(archive_read_disk_set_symlink_physical(disk.get()),
                        disk.get(), "set physical symlink");
    check_archive_throw(archive_read_disk_set_standard_lookup(disk.get()),
                        disk.get(), "set uid/gid name lookup");
    if (one_file_system)
        check_archive_throw(
            archive_read_disk_set_behavior(disk.get(),
                                           ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS),
            disk.get(), "set one-file-system");
    return disk;
}

ResolverHandle make_link_resolver() {
    ResolverHandle resolver(archive_entry_linkresolver_new(),
                            archive_entry_linkresolver_free);
    if (!resolver)
        throw std::runtime_error("cannot allocate hardlink resolver");
    ArchiveWriteHandle writer(archive_write_new());
    if (!writer.get())
        throw std::runtime_error("cannot allocate archive writer");
    check_archive_throw(archive_write_set_format_pax(writer.get()),
                        writer.get(), "set pax format");
    archive_entry_linkresolver_set_strategy(resolver.get(),
                                            archive_format(writer.get()));
    return resolver;
}

PaxWriteResult write_pax_archive(const Options &opts,
                                 PaxWriterCallbacks callbacks) {
    ensure_utf8_ctype_locale();
    if (!callbacks.write_chunk)
        callbacks.write_chunk = [](PaxChunk) {};
    if (opts.chdir_dir && chdir(opts.chdir_dir->c_str()) != 0)
        throw_errno(string("chdir ") + *opts.chdir_dir);

    auto resolver = make_link_resolver();
    ArchiveStats stats;
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    SlicePipeline slices(opts, callbacks, stats, hasher);
    std::optional<uint64_t> active_slice;
    std::atomic<uint64_t> slice_count{0};
    RateSampler rates;
    PeriodicProgress progress([&] {
        if (callbacks.progress_paused())
            return;
        auto input = stats.input_bytes.load();
        auto output = stats.output_bytes.load();
        auto rate = rates.sample(input, output, stats.walked_entries.load());
        print_pax_progress(
            rate.input, rate.output, rate.items, slice_count.load(), output,
            buffer_percent(slices.buffered_bytes(), slices.buffer_capacity()));
    });

    auto begin_slice = [&](uint64_t number) {
        if (active_slice && *active_slice == number)
            return;
        if (active_slice)
            slices.finish_slice();
        slices.begin_slice(number);
        active_slice = number;
        ++slice_count;
    };
    auto emit_linked = [&](archive_entry *raw) {
        archive_entry *spare = nullptr;
        archive_entry_linkify(resolver.get(), &raw, &spare);
        EntryHandle entry(raw), other(spare);
        if (entry)
            slices.emit_entry(std::move(entry));
        if (other)
            slices.emit_entry(std::move(other));
    };

    try {
        if (opts.plan_path) {
            PlanReader records(*opts.plan_path);
            auto disk = open_disk_reader(false);
            while (auto record = records.next()) {
                if (record->chdir_dir) {
                    if (chdir(record->chdir_dir->c_str()) != 0)
                        throw_errno(string("chdir ") + *record->chdir_dir);
                    continue;
                }
                const auto &entry = *record->entry;
                begin_slice(entry.slice);
                emit_linked(planned_entry_from_path(disk.get(), entry.path));
            }
        } else {
            begin_slice(0);
            for (const string &source : opts.sources) {
                auto spec = make_source_spec(source);
                auto disk = open_disk_reader(opts.one_file_system);
                check_archive_throw(
                    archive_read_disk_open(disk.get(), spec.open_path.c_str()),
                    disk.get(), "open source path");
                for (;;) {
                    EntryHandle entry(archive_entry_new());
                    if (!entry)
                        throw std::runtime_error("cannot allocate entry");
                    int r = archive_read_next_header2(disk.get(), entry.get());
                    if (r == ARCHIVE_EOF)
                        break;
                    if (r == ARCHIVE_FATAL)
                        throw_archive("read filesystem", disk.get());
                    if (r < ARCHIVE_OK) {
                        warn_archive("read filesystem", disk.get());
                        continue;
                    }
                    if (archive_read_disk_can_descend(disk.get())) {
                        r = archive_read_disk_descend(disk.get());
                        if (r == ARCHIVE_FATAL)
                            throw_archive("descend", disk.get());
                        if (r < ARCHIVE_OK)
                            warn_archive("descend", disk.get());
                    }
                    if (const char *path =
                            archive_entry_sourcepath(entry.get()))
                        copy_pathname(entry.get(),
                                      archive_path_for_source(spec, path));
                    emit_linked(entry.release());
                }
            }
        }
        for (;;) {
            archive_entry *entry = nullptr, *spare = nullptr;
            archive_entry_linkify(resolver.get(), &entry, &spare);
            if (!entry && !spare)
                break;
            EntryHandle linked(entry), other(spare);
            if (linked)
                slices.emit_entry(std::move(linked));
            if (other)
                slices.emit_entry(std::move(other));
        }
        if (active_slice) {
            slices.emit_eoa();
            slices.finish_slice();
        }
    } catch (...) {
        slices.abort();
        throw;
    }
    progress.stop();
    if (opts.plan_path) {
        print_pax_progress(0, 0, 0, slice_count.load(),
                           stats.output_bytes.load(), 0);
        finish_progress();
    }
    std::array<uint8_t, BLAKE3_OUT_LEN> hash{};
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());
    string hex;
    for (uint8_t byte : hash)
        hex += format("{:02x}", byte);
    return {stats.input_bytes.load(), stats.output_bytes.load(),
            stats.walked_entries.load(), slice_count.load(), hex};
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
