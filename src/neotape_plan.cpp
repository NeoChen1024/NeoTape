#include "neotape/common.hpp"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <exception>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// ====================== Planner State ============================

namespace fs = std::filesystem;
using std::format;
using std::string;
using std::string_view;
using std::vector;

struct Options {
	uint64_t slice_size = 64ull * 1024 * 1024 * 1024;
	uint64_t metadata_buffer_size = 256ull * 1024 * 1024;
	bool one_file_system = false;
	bool verbose = false;
	string output_path = "-";
	FILE *meta_out = nullptr;
	string chdir_dir;
	vector<fs::path> sources;
	unsigned io_threads = 1;
};

struct EntryMeta {
	fs::path source_path;
	string archive_path;
	char kind = '?';
	uint64_t disk_bytes = 0;
	uint64_t apparent_bytes = 0;
	dev_t device = 0;
};

struct SlicePlan {
	vector<EntryMeta> entries;
	uint64_t disk_bytes = 0;
	uint64_t apparent_bytes = 0;
	uint64_t allocated_bytes = 0;
};

struct ScanTotals {
	uint64_t entries = 0;
	uint64_t disk_bytes = 0;
	uint64_t apparent_bytes = 0;
};

// ====================== Diagnostics & CLI ========================

[[noreturn]] void fail(const string &message) {
	std::cerr << format("neotape-plan: {}\n", message);
	std::exit(1);
}

void warn(const string &message) {
	std::cerr << format("neotape-plan: warning: {}\n", message);
}

void usage(const char *prog) {
	std::cerr << format(
	    "usage: {} [--slice-size <bytes>] [--metadata-buffer-size <bytes>]\n"
	    "       -o <file> [-C <dir>] [-x] [-v]\n"
	    "       [--io-threads <N>] <path> [path ...]\n",
	    prog);
}

Options parse_args(int argc, char **argv) {
	static const struct option long_opts[] = {
		{"slice-size",           required_argument, nullptr, 's'},
		{"metadata-buffer-size", required_argument, nullptr, 'm'},
		{"output",               required_argument, nullptr, 'o'},
		{"directory",            required_argument, nullptr, 'C'},
		{"io-threads",           required_argument, nullptr, 256},
		{"help",                 no_argument,       nullptr, 'h'},
		{nullptr, 0, nullptr, 0}
	};

	Options opts;
	int c;
	while ((c = getopt_long(argc, argv, "C:o:xvh", long_opts, nullptr)) != -1) {
		switch (c) {
		case 's':
			opts.slice_size = neotape::parse_size(optarg, "slice size");
			break;
		case 'm':
			opts.metadata_buffer_size =
			    neotape::parse_size(optarg, "metadata buffer size");
			break;
		case 'o': opts.output_path = optarg; break;
		case 'C': opts.chdir_dir = optarg; break;
		case 'x': opts.one_file_system = true; break;
		case 'v': opts.verbose = true; break;
		case 256: {
			char *end = nullptr;
			unsigned long n = std::strtoul(optarg, &end, 10);
			if (end == optarg || *end != '\0')
				{ fail("--io-threads requires a number"); }
			opts.io_threads = static_cast<unsigned>(n);
			break;
		}
		case 'h': usage(argv[0]); std::exit(0);
		case '?': std::exit(2);
		}
	}

	while (optind < argc)
		opts.sources.emplace_back(argv[optind++]);

	if (opts.output_path.empty() || opts.sources.empty()) {
		usage(argv[0]);
		std::exit(2);
	}
	return opts;
}

// ====================== Filesystem Metadata ======================

char kind_from_mode(mode_t mode) {
	if (S_ISREG(mode))
		return 'f';
	if (S_ISDIR(mode))
		return 'd';
	if (S_ISLNK(mode))
		return 'l';
	if (S_ISCHR(mode))
		return 'c';
	if (S_ISBLK(mode))
		return 'b';
	if (S_ISFIFO(mode))
		return 'p';
	if (S_ISSOCK(mode))
		return 's';
	return '?';
}

uint64_t disk_bytes_from_stat(const struct stat &st) {
	if (st.st_blocks <= 0)
		return 0;
	return static_cast<uint64_t>(st.st_blocks) * 512;
}

uint64_t apparent_bytes_from_stat(const struct stat &st) {
	if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
		return st.st_size < 0 ? 0 : static_cast<uint64_t>(st.st_size);
	return 0;
}

vector<fs::path> sorted_children(const fs::path &path) {
	DIR *dir = opendir(path.c_str());
	if (dir == nullptr) {
		warn(format("opendir {}: {}", path.string(), std::strerror(errno)));
		return {};
	}

	vector<fs::path> children;
	for (;;) {
		errno = 0;
		dirent *entry = readdir(dir);
		if (entry == nullptr)
			break;
		string_view name(entry->d_name);
		if (name == "." || name == "..")
			continue;
		children.push_back(path / string(name));
	}
	if (errno != 0)
		warn(format("readdir {}: {}", path.string(), std::strerror(errno)));
	if (closedir(dir) != 0)
		warn(format("closedir {}: {}", path.string(), std::strerror(errno)));

	std::ranges::sort(children);
	return children;
}

// ====================== Thread Pool for lstat ====================

template<typename T>
class BlockingQueue {
	std::mutex mtx_;
	std::condition_variable not_empty_;
	std::condition_variable not_full_;
	std::queue<T> queue_;
	size_t capacity_ = 0;
	bool closed_ = false;
public:
	explicit BlockingQueue(size_t capacity = 0) : capacity_(capacity) {}

	bool push(T item) {
		std::unique_lock l(mtx_);
		not_full_.wait(l, [this] {
			return closed_ || capacity_ == 0 || queue_.size() < capacity_;
		});
		if (closed_)
			return false;
		queue_.push(std::move(item));
		not_empty_.notify_one();
		return true;
	}

	std::optional<T> pop() {
		std::unique_lock l(mtx_);
		not_empty_.wait(l, [this] { return !queue_.empty() || closed_; });
		if (queue_.empty()) return std::nullopt;
		T item = std::move(queue_.front());
		queue_.pop();
		not_full_.notify_one();
		return item;
	}

	void close() {
		std::lock_guard l(mtx_);
		closed_ = true;
		not_empty_.notify_all();
		not_full_.notify_all();
	}
};

struct LstatWork {
	size_t index;
	fs::path path;
	bool one_file_system = false;
	std::optional<dev_t> root_device;
	const neotape::SourceSpec *spec = nullptr;
};

struct LstatResult {
	size_t index;
	bool ok = false;
	EntryMeta meta;
	std::exception_ptr error;
	string warning;
};

class WorkerPool {
	BlockingQueue<LstatWork> jobs_;
	BlockingQueue<LstatResult> results_;
	std::vector<std::thread> workers_;
	size_t queue_capacity_ = 0;

	static LstatResult run_lstat(const LstatWork &w) {
		try {
			struct stat st {};
			if (lstat(w.path.c_str(), &st) != 0) {
				LstatResult r{};
				r.index = w.index;
				r.warning = format("lstat {}: {}",
				    w.path.string(), std::strerror(errno));
				return r;
			}

			if (w.one_file_system && w.root_device.has_value() &&
			    S_ISDIR(st.st_mode) && st.st_dev != *w.root_device) {
				LstatResult r{};
				r.index = w.index;
				return r;
			}

			LstatResult r{};
			r.index = w.index;
			r.ok = true;
			r.meta = EntryMeta{
			    .source_path = w.path,
			    .archive_path = neotape::archive_path_for_source(
			        *w.spec, w.path.generic_string()),
			    .kind = kind_from_mode(st.st_mode),
			    .disk_bytes = disk_bytes_from_stat(st),
			    .apparent_bytes = apparent_bytes_from_stat(st),
			    .device = st.st_dev,
			};
			return r;
		} catch (...) {
			LstatResult r{};
			r.index = w.index;
			r.error = std::current_exception();
			return r;
		}
	}

	void worker_loop() {
		for (;;) {
			auto opt = jobs_.pop();
			if (!opt) return;
			if (!results_.push(run_lstat(*opt)))
				return;
		}
	}

public:
	explicit WorkerPool(unsigned nworkers = 0)
	    : jobs_(nworkers == 0 ? 0 : static_cast<size_t>(nworkers) * 4),
	      results_(nworkers == 0 ? 0 : static_cast<size_t>(nworkers) * 4),
	      queue_capacity_(nworkers == 0 ? 0 : static_cast<size_t>(nworkers) * 4) {}

	~WorkerPool() { stop(); }

	void start(unsigned nworkers) {
		if (nworkers == 0) return;
		for (unsigned i = 0; i < nworkers; i++)
			workers_.emplace_back(&WorkerPool::worker_loop, this);
	}

	void submit(LstatWork w) {
		if (!jobs_.push(std::move(w)))
			throw std::runtime_error("worker pool is closed");
	}

	std::vector<LstatResult> collect(const vector<fs::path> &paths,
	    bool one_file_system, std::optional<dev_t> root_device,
	    const neotape::SourceSpec &spec) {
		size_t count = paths.size();
		std::map<size_t, LstatResult> pending;
		size_t submitted = 0;
		size_t received = 0;
		size_t window = queue_capacity_ == 0 ? count : queue_capacity_;

		auto submit_more = [&] {
			while (submitted < count && submitted - received < window) {
				submit(LstatWork{submitted, paths[submitted],
				    one_file_system, root_device, &spec});
				++submitted;
			}
		};

		submit_more();
		while (received < count) {
			auto r = results_.pop();
			if (!r)
				throw std::runtime_error(
				    "worker pool closed while waiting for lstat results");
			++received;
			if (r->error)
				std::rethrow_exception(r->error);
			if (!r->warning.empty())
				warn(r->warning);
			pending[r->index] = std::move(*r);
			submit_more();
		}

		std::vector<LstatResult> out;
		out.reserve(count);
		for (auto &[_, r] : pending)
			out.push_back(std::move(r));
		return out;
	}

	unsigned nworkers() const { return static_cast<unsigned>(workers_.size()); }

	void stop() {
		jobs_.close();
		results_.close();
		for (auto &t : workers_)
			if (t.joinable()) t.join();
		workers_.clear();
	}
};

// ====================== Streaming Slice Packing ==================

void emit_slice(const SlicePlan &slice, const Options &opts, uint64_t slice_num,
    vector<uint64_t> &slice_sizes) {
	for (size_t i = 0; i < slice.entries.size(); ++i) {
		const EntryMeta &e = slice.entries[i];
		string line = format("/{}/{}/{}/{}/{}", slice_num, i, e.kind,
		    e.apparent_bytes, e.archive_path);
		fwrite(line.data(), 1, line.size(), opts.meta_out);
		fputc('\0', opts.meta_out);
		fputc('\n', opts.meta_out);
	}
	std::cerr << format("slice {}: entries={} disk={} apparent={}\n",
	    slice_num, slice.entries.size(),
	    neotape::humanize_number(slice.disk_bytes),
	    neotape::humanize_number(slice.apparent_bytes));
	slice_sizes.push_back(slice.disk_bytes);

	if (!opts.verbose)
		return;
	for (const EntryMeta &entry : slice.entries) {
		std::cerr << format("  {} disk={} apparent={} {}\n", entry.kind,
		    neotape::humanize_number(entry.disk_bytes),
		    neotape::humanize_number(entry.apparent_bytes),
		    entry.source_path.generic_string());
	}
}

static constexpr uint64_t entry_struct_size = sizeof(EntryMeta);

void add_to_slice(SlicePlan &slice, const EntryMeta &entry,
    const Options &opts, uint64_t &slice_num, ScanTotals &totals,
    vector<uint64_t> &slice_sizes) {
	slice.entries.push_back(entry);
	slice.disk_bytes += entry.disk_bytes;
	slice.apparent_bytes += entry.apparent_bytes;

	auto &s = entry.source_path.native();
	uint64_t path_heap = s.size();
	if (path_heap <= 15)
		path_heap = 0;
	uint64_t ap_heap = entry.archive_path.size();
	if (ap_heap <= 15)
		ap_heap = 0;
	slice.allocated_bytes += entry_struct_size + path_heap + ap_heap;

	++totals.entries;
	totals.disk_bytes += entry.disk_bytes;
	totals.apparent_bytes += entry.apparent_bytes;

	if (slice.allocated_bytes >= opts.metadata_buffer_size ||
	    slice.disk_bytes >= opts.slice_size) {
		emit_slice(slice, opts, slice_num++, slice_sizes);
		slice = SlicePlan{};
	}
}

static constexpr size_t directory_frontier_capacity = 4096;

class PlannerScanner {
	struct DirectoryWork {
		fs::path path;
		const neotape::SourceSpec *spec = nullptr;
		std::optional<dev_t> root_device;
	};

	const Options &opts_;
	SlicePlan &current_slice_;
	uint64_t &slice_num_;
	ScanTotals &totals_;
	vector<uint64_t> &slice_sizes_;
	WorkerPool *pool_;
	std::deque<DirectoryWork> frontier_;

	LstatResult stat_child(size_t index, const fs::path &path,
	    const neotape::SourceSpec &spec, std::optional<dev_t> root_device) {
		struct stat st {};
		if (lstat(path.c_str(), &st) != 0) {
			LstatResult r{};
			r.index = index;
			r.warning = format("lstat {}: {}",
			    path.string(), std::strerror(errno));
			return r;
		}

		if (opts_.one_file_system && root_device.has_value() &&
		    S_ISDIR(st.st_mode) && st.st_dev != *root_device) {
			LstatResult r{};
			r.index = index;
			return r;
		}

		LstatResult r{};
		r.index = index;
		r.ok = true;
		r.meta = EntryMeta{
		    .source_path = path,
		    .archive_path = neotape::archive_path_for_source(
		        spec, path.generic_string()),
		    .kind = kind_from_mode(st.st_mode),
		    .disk_bytes = disk_bytes_from_stat(st),
		    .apparent_bytes = apparent_bytes_from_stat(st),
		    .device = st.st_dev,
		};
		return r;
	}

	vector<LstatResult> stat_children(const vector<fs::path> &children,
	    const neotape::SourceSpec &spec, std::optional<dev_t> root_device) {
		if (pool_ && pool_->nworkers() > 0)
			return pool_->collect(children, opts_.one_file_system,
			    root_device, spec);

		vector<LstatResult> results;
		results.reserve(children.size());
		for (size_t i = 0; i < children.size(); ++i)
			results.push_back(stat_child(i, children[i], spec, root_device));
		return results;
	}

	void enqueue_directory(fs::path path, const neotape::SourceSpec &spec,
	    std::optional<dev_t> root_device) {
		while (frontier_.size() >= directory_frontier_capacity)
			process_one_directory();
		frontier_.push_back(DirectoryWork{
		    .path = std::move(path),
		    .spec = &spec,
		    .root_device = root_device,
		});
	}

	void process_one_directory() {
		DirectoryWork dir = std::move(frontier_.front());
		frontier_.pop_front();

		vector<fs::path> children = sorted_children(dir.path);
		if (children.empty())
			return;

		for (auto &r : stat_children(children, *dir.spec, dir.root_device)) {
			if (!r.warning.empty())
				warn(r.warning);
			if (!r.ok)
				continue;

			add_to_slice(current_slice_, r.meta,
			    opts_, slice_num_, totals_, slice_sizes_);
			if (r.meta.kind == 'd')
				enqueue_directory(r.meta.source_path, *dir.spec,
				    dir.root_device);
		}
	}

public:
	PlannerScanner(const Options &opts, SlicePlan &current_slice,
	    uint64_t &slice_num, ScanTotals &totals,
	    vector<uint64_t> &slice_sizes, WorkerPool *pool)
	    : opts_(opts),
	      current_slice_(current_slice),
	      slice_num_(slice_num),
	      totals_(totals),
	      slice_sizes_(slice_sizes),
	      pool_(pool) {}

	void scan_source(const neotape::SourceSpec &spec) {
		struct stat st {};
		if (lstat(spec.open_path.c_str(), &st) != 0)
			fail(format("lstat {}: {}", spec.open_path.string(),
			    std::strerror(errno)));

		std::optional<dev_t> root_device = st.st_dev;
		add_to_slice(current_slice_,
		    EntryMeta{
		        .source_path = spec.open_path,
		        .archive_path = neotape::archive_path_for_source(
		            spec, spec.open_path.generic_string()),
		        .kind = kind_from_mode(st.st_mode),
		        .disk_bytes = disk_bytes_from_stat(st),
		        .apparent_bytes = apparent_bytes_from_stat(st),
		        .device = st.st_dev,
		    },
		    opts_, slice_num_, totals_, slice_sizes_);

		if (!S_ISDIR(st.st_mode))
			return;

		enqueue_directory(spec.open_path, spec, root_device);
		while (!frontier_.empty())
			process_one_directory();
	}
};

} // namespace

int main(int argc, char **argv) {
	try {
		Options opts = parse_args(argc, argv);

		if (opts.output_path == "-") {
			opts.meta_out = stdout;
		} else {
			opts.meta_out = fopen(opts.output_path.c_str(), "wb");
			if (!opts.meta_out)
				fail(format("open {}: {}", opts.output_path,
				    std::strerror(errno)));
		}

		if (!opts.chdir_dir.empty()) {
			if (chdir(opts.chdir_dir.c_str()) != 0)
				fail(format("chdir {}: {}", opts.chdir_dir,
				    std::strerror(errno)));
			string line = format("/chdir/{}", opts.chdir_dir);
			fwrite(line.data(), 1, line.size(), opts.meta_out);
			fputc('\0', opts.meta_out);
			fputc('\n', opts.meta_out);
		}

		vector<neotape::SourceSpec> sources;
		for (const fs::path &source : opts.sources)
			sources.push_back(neotape::make_source_spec(source.generic_string()));

		vector<uint64_t> slice_sizes;
		SlicePlan current_slice;
		ScanTotals totals;
		uint64_t slice_num = 0;

		unsigned nworkers = opts.io_threads > 0 ? opts.io_threads - 1 : 0;
		WorkerPool pool(nworkers);
		pool.start(nworkers);

		PlannerScanner scanner(opts, current_slice, slice_num,
		    totals, slice_sizes, nworkers > 0 ? &pool : nullptr);
		for (const neotape::SourceSpec &spec : sources)
			scanner.scan_source(spec);

		pool.stop();

		if (!current_slice.entries.empty())
			emit_slice(current_slice, opts, slice_num++, slice_sizes);

		if (opts.meta_out && opts.meta_out != stdout)
			fclose(opts.meta_out);

		std::cerr << format(
		    "scanned entries={} total_disk={} total_apparent={} "
		    "target_slice={} buffer_size={}\n",
		    totals.entries,
		    neotape::humanize_number(totals.disk_bytes),
		    neotape::humanize_number(totals.apparent_bytes),
		    neotape::humanize_number(opts.slice_size),
		    neotape::humanize_number(opts.metadata_buffer_size));

		if (!slice_sizes.empty()) {
			uint64_t min_sz = slice_sizes[0], max_sz = slice_sizes[0];
			uint64_t sum_sz = 0;
			for (uint64_t sz : slice_sizes) {
				if (sz < min_sz) min_sz = sz;
				if (sz > max_sz) max_sz = sz;
				sum_sz += sz;
			}
			double avg = static_cast<double>(sum_sz) / slice_sizes.size();
			double var_sum = 0;
			for (uint64_t sz : slice_sizes) {
				double d = static_cast<double>(sz) - avg;
				var_sum += d * d;
			}
			double stddev = std::sqrt(var_sum / slice_sizes.size());
			std::cerr << format(
			    "slice_sizes: slices={} min={} avg={} max={} "
			    "stddev={}\n",
			    slice_sizes.size(),
			    neotape::humanize_number(min_sz),
			    neotape::humanize_number(static_cast<uint64_t>(avg)),
			    neotape::humanize_number(max_sz),
			    neotape::humanize_number(static_cast<uint64_t>(stddev)));
		}

	} catch (const std::exception &e) {
		fail(e.what());
	}

	return 0;
}
