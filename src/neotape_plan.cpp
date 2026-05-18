#include "neotape/common.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
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
	bool one_file_system = false;
	bool verbose = false;
	vector<fs::path> sources;
};

struct EntryMeta {
	fs::path path;
	char kind = '?';
	uint64_t disk_bytes = 0;
	uint64_t apparent_bytes = 0;
	dev_t device = 0;
};

struct SlicePlan {
	vector<std::size_t> entry_indexes;
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
	    "usage: {} [--slice-size <bytes>] [-x] [-v] <path> [path ...]\n", prog);
}

Options parse_args(int argc, char **argv) {
	Options opts;
	for (int i = 1; i < argc; ++i) {
		string_view arg(argv[i]);
		auto need_value = [&](const char *name) -> string {
			if (++i >= argc)
				fail(format("{} requires a value", name));
			return argv[i];
		};

		if (arg == "--slice-size") {
			opts.slice_size = neotape::parse_size(need_value("--slice-size"), "slice size");
		} else if (arg == "-x") {
			opts.one_file_system = true;
		} else if (arg == "-v") {
			opts.verbose = true;
		} else if (arg == "-h" || arg == "--help") {
			usage(argv[0]);
			std::exit(0);
		} else if (!arg.empty() && arg.front() == '-') {
			fail(format("unknown option: {}", arg));
		} else {
			opts.sources.emplace_back(arg);
		}
	}

	if (opts.sources.empty()) {
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

fs::path normalized_path(const fs::path &path) {
	std::error_code ec;
	fs::path absolute = fs::absolute(path, ec);
	if (ec)
		return path;
	return absolute.lexically_normal();
}

void add_entry(const fs::path &path, const struct stat &st, vector<EntryMeta> &entries) {
	entries.push_back(EntryMeta{
	    .path = path,
	    .kind = kind_from_mode(st.st_mode),
	    .disk_bytes = disk_bytes_from_stat(st),
	    .apparent_bytes = apparent_bytes_from_stat(st),
	    .device = st.st_dev,
	});
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

void scan_path(const fs::path &path, const Options &opts,
    std::optional<dev_t> root_device, vector<EntryMeta> &entries) {
	struct stat st {};
	if (lstat(path.c_str(), &st) != 0) {
		warn(format("lstat {}: {}", path.string(), std::strerror(errno)));
		return;
	}

	if (opts.one_file_system && root_device.has_value() && S_ISDIR(st.st_mode) &&
	    st.st_dev != *root_device)
		return;

	add_entry(path, st, entries);
	if (!S_ISDIR(st.st_mode))
		return;

	for (const fs::path &child : sorted_children(path))
		scan_path(child, opts, root_device, entries);
}

vector<EntryMeta> prefetch_metadata(const Options &opts) {
	vector<EntryMeta> entries;
	for (const fs::path &source : opts.sources) {
		fs::path path = normalized_path(source);
		struct stat st {};
		if (lstat(path.c_str(), &st) != 0)
			fail(format("lstat {}: {}", path.string(), std::strerror(errno)));
		scan_path(path, opts, st.st_dev, entries);
	}
	return entries;
}

// ====================== Slice Packing ============================

void add_to_slice(SlicePlan &slice, std::size_t index, const EntryMeta &entry) {
	slice.entry_indexes.push_back(index);
	slice.disk_bytes += entry.disk_bytes;
	slice.apparent_bytes += entry.apparent_bytes;
}

vector<SlicePlan> pack_slices(const vector<EntryMeta> &entries, uint64_t slice_size) {
	vector<SlicePlan> slices;
	SlicePlan current;

	for (std::size_t i = 0; i < entries.size(); ++i) {
		const EntryMeta &entry = entries[i];
		bool would_exceed = current.disk_bytes > 0 &&
				    current.disk_bytes + entry.disk_bytes > slice_size;
		if (!current.entry_indexes.empty() && would_exceed) {
			slices.push_back(std::move(current));
			current = SlicePlan{};
		}
		add_to_slice(current, i, entry);
	}

	if (!current.entry_indexes.empty())
		slices.push_back(std::move(current));
	return slices;
}

void print_plan(const Options &opts, const vector<EntryMeta> &entries,
    const vector<SlicePlan> &slices) {
	uint64_t total_disk = 0;
	uint64_t total_apparent = 0;
	for (const EntryMeta &entry : entries) {
		total_disk += entry.disk_bytes;
		total_apparent += entry.apparent_bytes;
	}

	std::cout << format(
	    "prefetched entries={} total_disk={} total_apparent={} target_slice={}\n",
	    entries.size(), neotape::humanize_number(total_disk),
	    neotape::humanize_number(total_apparent), neotape::humanize_number(opts.slice_size));

	for (std::size_t i = 0; i < slices.size(); ++i) {
		const SlicePlan &slice = slices[i];
		std::cout << format("slice {:06}: entries={} disk={} apparent={}\n",
		    i + 1, slice.entry_indexes.size(),
		    neotape::humanize_number(slice.disk_bytes),
		    neotape::humanize_number(slice.apparent_bytes));
		if (!opts.verbose)
			continue;
		for (std::size_t entry_index : slice.entry_indexes) {
			const EntryMeta &entry = entries[entry_index];
			std::cout << format("  {} disk={} apparent={} {}\n", entry.kind,
			    neotape::humanize_number(entry.disk_bytes),
			    neotape::humanize_number(entry.apparent_bytes), entry.path.generic_string());
		}
	}
}

} // namespace

int main(int argc, char **argv) {
	try {
		Options opts = parse_args(argc, argv);
		vector<EntryMeta> entries = prefetch_metadata(opts);
		vector<SlicePlan> slices = pack_slices(entries, opts.slice_size);
		print_plan(opts, entries, slices);
		return 0;
	} catch (const std::exception &e) {
		fail(e.what());
	}
}
