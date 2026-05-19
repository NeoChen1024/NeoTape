#include "neotape/bounded_buffer.hpp"
#include "neotape/common.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <blake3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using neotape::BoundedBuffer;
using std::format;
using std::size_t;
using std::string;
using std::vector;

constexpr size_t SMALL_FILE_THRESHOLD = 4UL * 1024 * 1024;
constexpr size_t DEFAULT_OUTPUT_BUFFER_SIZE = 64UL * 1024 * 1024;
constexpr size_t BB0_CAPACITY_ENTRIES = 64UL * 1024;
constexpr size_t COMPLETED_QUEUE_MULTIPLIER = 2;
constexpr size_t PAX_ENTRY_OVERHEAD_RESERVE = 16UL * 1024;
constexpr size_t STREAM_FLUSH_THRESH = 4UL * 1024 * 1024;

// ========================== Types ==========================

struct Options {
	string output;
	vector<string> sources;
	int verbose = 0;
	bool one_file_system = false;
	std::optional<string> chdir_dir;
	size_t output_buf_size = DEFAULT_OUTPUT_BUFFER_SIZE;
	unsigned io_thread = 1;
};

struct Result {
	uint64_t seq;
	std::vector<std::byte> bytes;  // empty is a valid skipped/warned entry
};

struct WorkItem {
	uint64_t seq;
	archive_entry *entry;
	int fd;  // -1 = no data
};

enum class SlotState : uint8_t { IDLE, BUSY };

// ── BBSink: streaming accumulator to BoundedBuffer ──

struct BBSink {
	BoundedBuffer *dest;
	std::vector<std::byte> accum;
	bool drop_mode = false;
};

la_ssize_t bb_sink_write(archive *, void *client, const void *data, size_t len) {
	auto *sink = static_cast<BBSink *>(client);
	if (sink->drop_mode)
		return static_cast<la_ssize_t>(len);

	auto *bytes = static_cast<const std::byte *>(data);
	sink->accum.insert(sink->accum.end(), bytes, bytes + len);
	if (sink->accum.size() >= STREAM_FLUSH_THRESH) {
		if (!sink->dest->push(std::move(sink->accum)))
			return -1;
		sink->accum = {};
		sink->accum.reserve(STREAM_FLUSH_THRESH);
	}
	return static_cast<la_ssize_t>(len);
}

int bb_sink_close(archive *, void *) { return ARCHIVE_OK; }

// ====================== Diagnostics ==========================

void usage(const char *prog) {
	std::cerr << format(
	    "usage: {} -f <out-file|-> [-v|-vv] [-x] [-C <dir>]\n"
	    "       [--io-thread <N>] [--output-buffer-size <bytes>] <path> [path ...]\n", prog);
}

[[noreturn]] void fail_archive(const char *context, archive *a) {
	const char *msg = archive_error_string(a);
	std::cerr << format("pax: {}{}\n", context,
	    msg != nullptr ? format(": {}", msg) : string());
	std::exit(1);
}

[[noreturn]] void fail_errno(const string &context) {
	std::cerr << format("pax: {}: {}\n", context, std::strerror(errno));
	std::exit(1);
}

void check_archive(int r, archive *a, const char *context) {
	if (r == ARCHIVE_OK) return;
	if (r == ARCHIVE_WARN) {
		const char *msg = archive_error_string(a);
		std::cerr << format("pax: warning: {}{}\n", context,
		    msg != nullptr ? format(": {}", msg) : string());
		return;
	}
	fail_archive(context, a);
}

void warn_archive(const char *context, archive *a) {
	const char *msg = archive_error_string(a);
	std::cerr << format("pax: warning: {}{}\n", context,
	    msg != nullptr ? format(": {}", msg) : string());
}

bool locale_name_is_utf8(const char *name) {
	if (name == nullptr) return false;
	string locale_name(name);
	std::ranges::transform(locale_name, locale_name.begin(),
	    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return locale_name.find("utf-8") != string::npos ||
	       locale_name.find("utf8") != string::npos;
}

void ensure_utf8_ctype_locale() {
	const char *locale_name = std::setlocale(LC_CTYPE, "");
	if (locale_name_is_utf8(locale_name)) return;
	for (const char *fb : {"C.UTF-8", "en_US.UTF-8"}) {
		locale_name = std::setlocale(LC_CTYPE, fb);
		if (locale_name_is_utf8(locale_name)) return;
	}
}

// ====================== Entry Formatting =====================

void mark_link_target_as_utf8(archive_entry *entry) {
	if (const char *s = archive_entry_symlink(entry); s != nullptr)
		archive_entry_update_symlink_utf8(entry, s);
	else if (const char *h = archive_entry_hardlink(entry); h != nullptr)
		archive_entry_update_hardlink_utf8(entry, h);
}

string entry_owner_name(archive_entry *entry) {
	if (const char *n = archive_entry_uname(entry); n != nullptr) return n;
	return std::to_string(archive_entry_uid(entry));
}

string entry_group_name(archive_entry *entry) {
	if (const char *n = archive_entry_gname(entry); n != nullptr) return n;
	return std::to_string(archive_entry_gid(entry));
}

string entry_timestamp(archive_entry *entry) {
	std::time_t t = archive_entry_mtime(entry);
	std::tm lt{};
	if (localtime_r(&t, &lt) == nullptr) return "00000000T000000+0000";
	char buf[32]{};
	if (std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%S%z", &lt) == 0)
		return "00000000T000000+0000";
	return buf;
}

string entry_display_path(archive_entry *entry) {
	string p = archive_entry_pathname(entry) != nullptr
	    ? archive_entry_pathname(entry) : "";
	if (archive_entry_filetype(entry) == AE_IFDIR && !p.empty() && p.back() != '/')
		p += '/';
	return p;
}

string entry_size_display(archive_entry *entry) {
	la_int64_t sz = archive_entry_size(entry);
	if (sz < 0) return "?";
	return neotape::humanize_number(static_cast<size_t>(sz));
}

string verbose_line(archive_entry *entry) {
	string mode = archive_entry_strmode(entry);
	if (mode.size() > 10) mode.resize(10);
	if (archive_entry_hardlink(entry) != nullptr && !mode.empty())
		mode[0] = 'h';
	return format("{} {:3} {:>10} {:>10} {:>6} [{}] {}", mode,
	    archive_entry_nlink(entry), entry_owner_name(entry), entry_group_name(entry),
	    entry_size_display(entry), entry_timestamp(entry), entry_display_path(entry));
}

int open_entry_file(archive_entry *entry) {
	const char *src = archive_entry_sourcepath(entry);
	if (src == nullptr) src = archive_entry_pathname(entry);
	if (src == nullptr) return -1;
	int fd = open(src, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		std::cerr << format("pax: warning: open {}: {}\n", src, std::strerror(errno));
	return fd;
}

// ====================== Data Copying =========================

void copy_file_data(archive *writer, archive_entry *entry, int fd) {
	const char *src = archive_entry_sourcepath(entry);
	if (src == nullptr) src = archive_entry_pathname(entry);
	if (src == nullptr) fail_archive("entry has no source path", writer);

	thread_local vector<char> buf;
	if (buf.empty())
		buf.resize(SMALL_FILE_THRESHOLD);
	for (;;) {
		ssize_t n = read(fd, buf.data(), buf.size());
		if (n < 0) fail_errno(string("read ") + src);
		if (n == 0) break;
		ssize_t w = archive_write_data(writer, buf.data(), static_cast<size_t>(n));
		if (w < 0) fail_archive("write file data", writer);
		if (w != n) {
			std::cerr << format("pax: short archive write for {}\n", src);
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
	if (ctx->drop) return static_cast<la_ssize_t>(len);
	auto *bytes = static_cast<const std::byte *>(data);
	ctx->buf.insert(ctx->buf.end(), bytes, bytes + len);
	return static_cast<la_ssize_t>(len);
}

int drop_close(archive *, void *) { return ARCHIVE_OK; }

vector<std::byte> serialize_entry(archive_entry *entry, int fd) {
	archive *a = archive_write_new();
	if (!a) { std::cerr << "pax: cannot allocate archive writer\n"; std::exit(1); }
	check_archive(archive_write_add_filter_none(a), a, "set uncompressed");
	check_archive(archive_write_set_format_pax(a), a, "set pax format");
	check_archive(archive_write_set_options(a, "xattrheader=ALL,hdrcharset=UTF-8"),
	    a, "set options");
	check_archive(archive_write_set_bytes_per_block(a, 512), a, "set block size");
	check_archive(archive_write_set_bytes_in_last_block(a, 1), a, "set last block");

	BufCtx ctx;
	la_int64_t entry_size = archive_entry_size(entry);
	if (fd >= 0 && entry_size > 0) {
		size_t reserve_size = static_cast<size_t>(entry_size) +
		    PAX_ENTRY_OVERHEAD_RESERVE;
		ctx.buf.reserve(reserve_size);
	} else {
		ctx.buf.reserve(PAX_ENTRY_OVERHEAD_RESERVE);
	}
	check_archive(archive_write_open(a, &ctx, drop_open, drop_write, drop_close),
	    a, "open per-entry writer");

	int r = archive_write_header(a, entry);
	if (r == ARCHIVE_FATAL) fail_archive("write header", a);
	if (r < ARCHIVE_OK) {
		warn_archive("write header", a);
		ctx.drop = true;
		archive_write_close(a);
		archive_write_free(a);
		return {};
	}
	if (fd >= 0) copy_file_data(a, entry, fd);
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
	if (!a) { std::cerr << "pax: cannot allocate archive writer\n"; std::exit(1); }
	check_archive(archive_write_add_filter_none(a), a, "set uncompressed");
	check_archive(archive_write_set_format_pax(a), a, "set pax format");
	check_archive(archive_write_set_options(a, "xattrheader=ALL,hdrcharset=UTF-8"),
	    a, "set options");
	check_archive(archive_write_set_bytes_per_block(a, 512), a, "set block size");
	check_archive(archive_write_set_bytes_in_last_block(a, 1), a, "set last block");
	check_archive(archive_write_open(a, &sink, drop_open, bb_sink_write, bb_sink_close),
	    a, "open streaming writer");

	int r = archive_write_header(a, entry);
	if (r == ARCHIVE_FATAL) fail_archive("write header", a);
	if (r < ARCHIVE_OK) {
		warn_archive("write header", a);
		sink.drop_mode = true;
		archive_write_close(a);
		archive_write_free(a);
		return;
	}
	copy_file_data(a, entry, fd);
	check_archive(archive_write_finish_entry(a), a, "finish entry");

	if (!sink.accum.empty())
		sink.dest->push(std::move(sink.accum));

	sink.drop_mode = true;
	archive_write_close(a);
	archive_write_free(a);
}

// ====================== BlockingQueue ========================

template<typename T>
class BlockingQueue {
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
			cv_.wait(l, [this] { return queue_.size() < max_size_ || closed_; });
		if (closed_) return;
		queue_.push(std::move(item));
		cv_.notify_one();
	}
	std::optional<T> pop() {
		std::unique_lock l(mtx_);
		cv_.wait(l, [this] { return !queue_.empty() || closed_; });
		if (queue_.empty()) return std::nullopt;
		T item = std::move(queue_.front());
		queue_.pop();
		cv_.notify_one();
		return item;
	}
	std::optional<T> try_pop() {
		std::lock_guard l(mtx_);
		if (queue_.empty()) return std::nullopt;
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
    BlockingQueue<Result> &completed_queue, BlockingQueue<size_t> &idle_queue,
    std::mutex &notify_mtx, std::condition_variable &notify_cv,
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
		if (w.fd >= 0) close(w.fd);

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
    BlockingQueue<Result> &bb0, BlockingQueue<Result> &completed_queue,
    std::mutex &notify_mtx, std::condition_variable &notify_cv,
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
			if (!r.bytes.empty())
				bb1_sink.dest->push(std::move(r.bytes));
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
		if (w.fd >= 0) close(w.fd);
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
		notify_cv.wait(l, [&] {
			return notify_generation != seen_generation;
		});
		seen_generation = notify_generation;
	}
}

// ====================== Command-Line Parsing =================

Options parse_args(int argc, char **argv) {
	static const struct option long_opts[] = {
		{"directory",         required_argument, nullptr, 'C'},
		{"io-thread",         required_argument, nullptr, 257},
		{"output-buffer-size",required_argument, nullptr, 256},
		{"help",              no_argument,       nullptr, 'h'},
		{nullptr, 0, nullptr, 0}
	};

	Options opts;
	int c;
	while ((c = getopt_long(argc, argv, "C:f:vxh", long_opts, nullptr)) != -1) {
		switch (c) {
		case 'C': opts.chdir_dir = optarg; break;
		case 'f': opts.output = optarg; break;
		case 'v': opts.verbose = std::min(opts.verbose + 1, 2); break;
		case 'x': opts.one_file_system = true; break;
		case 256:
			try {
				opts.output_buf_size = static_cast<size_t>(
				    neotape::parse_size(optarg, "output buffer size"));
			} catch (const std::exception &e) {
				std::cerr << format("pax: {}\n", e.what());
				std::exit(2);
			}
			break;
		case 257: {
			char *end = nullptr;
			unsigned long n = std::strtoul(optarg, &end, 10);
			if (end == optarg || *end != '\0')
				{ std::cerr << "pax: --io-thread requires a number\n"; std::exit(2); }
			opts.io_thread = static_cast<unsigned>(n);
			break;
		}
		case 'h': usage(argv[0]); std::exit(0);
		case '?': std::exit(2);
		}
	}

	while (optind < argc)
		opts.sources.emplace_back(argv[optind++]);

	if (opts.output.empty() || opts.sources.empty()) {
		usage(argv[0]);
		std::exit(2);
	}
	return opts;
}

// ====================== Archive Emission =====================

void write_pax_archive(const Options &opts) {
	// ── Output file ──
	FILE *out_file;
	bool close_file;
	if (opts.output == "-") {
		out_file = stdout;
		close_file = false;
	} else {
		out_file = fopen(opts.output.c_str(), "wb");
		if (!out_file) fail_errno(string("open ") + opts.output);
		close_file = true;
	}

	// ── Bounded buffers ──
	BoundedBuffer bb1(opts.output_buf_size);
	BlockingQueue<Result> bb0(BB0_CAPACITY_ENTRIES);

	// ── Output thread ──
	blake3_hasher hasher;
	blake3_hasher_init(&hasher);
	std::atomic<bool> output_error{false};

	std::thread output_thread([&] {
		for (;;) {
			auto chunk = bb1.pop();
			if (chunk.empty()) break;
			if (fwrite(chunk.data(), 1, chunk.size(), out_file) != chunk.size()) {
				std::cerr << format("pax: write error: {}\n", std::strerror(errno));
				output_error.store(true);
				break;
			}
			blake3_hasher_update(&hasher, chunk.data(), chunk.size());
		}
		if (close_file && out_file) fclose(out_file);
	});

	// ── Worker slots ──
	unsigned nworkers = opts.io_thread > 0 ? opts.io_thread - 1 : 0;
	vector<WorkerSlot> slots(nworkers + 1);  // last slot = large
	size_t result_slots = std::max<size_t>(1, COMPLETED_QUEUE_MULTIPLIER * nworkers);
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
	BBSink bb1_sink{&bb1, {}, false};

	// ── Start workers ──
	vector<std::thread> worker_threads;
	for (size_t i = 0; i < nworkers; i++)
		worker_threads.emplace_back(worker_main,
		    i, std::ref(slots[i]),
		    std::ref(completed_queue), std::ref(idle_queue),
		    std::ref(notify_mtx), std::ref(notify_cv),
		    std::ref(notify_generation), std::ref(done));

	// ── Start serializer ──
	std::thread serializer_thread(serializer_main,
	    std::ref(slots), std::ref(bb1_sink),
	    std::ref(bb0), std::ref(completed_queue),
	    std::ref(notify_mtx), std::ref(notify_cv),
	    std::ref(notify_generation), std::ref(done));

	// ── Chdir ──
	if (opts.chdir_dir.has_value() && chdir(opts.chdir_dir->c_str()) != 0)
		fail_errno(string("chdir ") + *opts.chdir_dir);

	// ── Hardlink resolver ──
	archive_entry_linkresolver *resolver = archive_entry_linkresolver_new();
	if (!resolver) { std::cerr << "pax: cannot allocate hardlink resolver\n"; std::exit(1); }
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
			slots[idx].cv.wait(l, [&] {
				return slots[idx].state == SlotState::IDLE;
			});
			slots[idx].work = WorkItem{seq, entry, fd};
			slots[idx].state = SlotState::BUSY;
		}
		notify();
	};

	auto dispatch_entry = [&](archive_entry *entry) {
		if (!entry) return;

		bool is_reg = (archive_entry_filetype(entry) == AE_IFREG);
		la_int64_t size = archive_entry_size(entry);
		bool has_data = (is_reg && size > 0);

		if (opts.verbose > 1)
			std::cerr << format("{}\n", verbose_line(entry));
		else if (opts.verbose > 0)
			std::cerr << format("a {}\n", entry_display_path(entry));

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
			std::cerr << format("pax: {}\n", e.what());
			std::exit(1);
		}

		archive *disk = archive_read_disk_new();
		if (!disk) { std::cerr << "pax: cannot allocate disk reader\n"; std::exit(1); }
		check_archive(archive_read_disk_set_symlink_physical(disk), disk,
		    "set physical symlink");
		if (opts.one_file_system)
			check_archive(archive_read_disk_set_behavior(disk,
			    ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS), disk,
			    "set one-file-system");
		check_archive(archive_read_disk_set_standard_lookup(disk), disk,
		    "set uid/gid name lookup");
		check_archive(archive_read_disk_open(disk, spec.open_path.c_str()), disk,
		    "open source path");

		for (;;) {
			archive_entry *entry = archive_entry_new();
			if (!entry) { std::cerr << "pax: cannot allocate entry\n"; std::exit(1); }

			int r = archive_read_next_header2(disk, entry);
			if (r == ARCHIVE_EOF) { archive_entry_free(entry); break; }
			if (r == ARCHIVE_FATAL) fail_archive("read filesystem", disk);
			if (r < ARCHIVE_OK) { warn_archive("read filesystem", disk); archive_entry_free(entry); continue; }

			if (archive_read_disk_can_descend(disk)) {
				r = archive_read_disk_descend(disk);
				if (r == ARCHIVE_FATAL) fail_archive("descend", disk);
				if (r < ARCHIVE_OK) warn_archive("descend", disk);
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
		if (!entry && !spare) break;
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
		if (t.joinable()) t.join();
	if (serializer_thread.joinable()) serializer_thread.join();

	bb1.close();
	output_thread.join();

	if (output_error.load()) std::exit(1);

	// ── BLAKE3 ──
	std::array<uint8_t, BLAKE3_OUT_LEN> hash{};
	blake3_hasher_finalize(&hasher, hash.data(), hash.size());
	string hex;
	for (uint8_t b : hash) hex += format("{:02x}", static_cast<unsigned>(b));
	std::cerr << format("{}  {}\n", hex, opts.output == "-" ? "-" : opts.output);
}

} // namespace

int main(int argc, char **argv) {
	ensure_utf8_ctype_locale();
	Options opts = parse_args(argc, argv);
	write_pax_archive(opts);
	return 0;
}
