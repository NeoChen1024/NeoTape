#include "neotape/common.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <format>
#include <getopt.h>
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
	uint64_t metadata_buffer_size = 256ull * 1024 * 1024;
	bool one_file_system = false;
	bool verbose = false;
	string output_path = "-";
	FILE *meta_out = nullptr;
	vector<fs::path> sources;
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
	    "       -o <file> [-x] [-v] <path> [path ...]\n",
	    prog);
}

Options parse_args(int argc, char **argv) {
	static const struct option long_opts[] = {
		{"slice-size",           required_argument, nullptr, 's'},
		{"metadata-buffer-size", required_argument, nullptr, 'm'},
		{"output",               required_argument, nullptr, 'o'},
		{"help",                 no_argument,       nullptr, 'h'},
		{nullptr, 0, nullptr, 0}
	};

	Options opts;
	int c;
	while ((c = getopt_long(argc, argv, "o:xvh", long_opts, nullptr)) != -1) {
		switch (c) {
		case 's':
			opts.slice_size = neotape::parse_size(optarg, "slice size");
			break;
		case 'm':
			opts.metadata_buffer_size =
			    neotape::parse_size(optarg, "metadata buffer size");
			break;
		case 'o': opts.output_path = optarg; break;
		case 'x': opts.one_file_system = true; break;
		case 'v': opts.verbose = true; break;
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

// ====================== Streaming Slice Packing ==================

void emit_slice(const SlicePlan &slice, const Options &opts, uint64_t slice_num) {
	for (size_t i = 0; i < slice.entries.size(); ++i) {
		const EntryMeta &e = slice.entries[i];
		string line = format("/{:06}/{:06}/{}/{}/{}", slice_num, i, e.kind,
		    e.apparent_bytes, e.archive_path);
		fwrite(line.data(), 1, line.size(), opts.meta_out);
		fputc('\0', opts.meta_out);
		fputc('\n', opts.meta_out);
	}
	std::cerr << format("slice {:06}: entries={} disk={} apparent={}\n",
	    slice_num, slice.entries.size(),
	    neotape::humanize_number(slice.disk_bytes),
	    neotape::humanize_number(slice.apparent_bytes));
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
    const Options &opts, uint64_t &slice_num, ScanTotals &totals) {
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
		emit_slice(slice, opts, ++slice_num);
		slice = SlicePlan{};
	}
}

void scan_path(const fs::path &path, const Options &opts,
    const neotape::SourceSpec &spec, std::optional<dev_t> root_device,
    SlicePlan &current_slice, uint64_t &slice_num, ScanTotals &totals) {
	struct stat st {};
	if (lstat(path.c_str(), &st) != 0) {
		warn(format("lstat {}: {}", path.string(), std::strerror(errno)));
		return;
	}

	if (opts.one_file_system && root_device.has_value() && S_ISDIR(st.st_mode) &&
	    st.st_dev != *root_device)
		return;

	add_to_slice(current_slice,
	    EntryMeta{
	        .source_path = path,
	        .archive_path = neotape::archive_path_for_source(spec, path.generic_string()),
	        .kind = kind_from_mode(st.st_mode),
	        .disk_bytes = disk_bytes_from_stat(st),
	        .apparent_bytes = apparent_bytes_from_stat(st),
	        .device = st.st_dev,
	    },
	    opts, slice_num, totals);

	if (!S_ISDIR(st.st_mode))
		return;

	for (const fs::path &child : sorted_children(path))
		scan_path(child, opts, spec, root_device, current_slice, slice_num, totals);
}

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

		vector<neotape::SourceSpec> sources;
		for (const fs::path &source : opts.sources)
			sources.push_back(neotape::make_source_spec(source.generic_string()));

		SlicePlan current_slice;
		ScanTotals totals;
		uint64_t slice_num = 0;

		for (const neotape::SourceSpec &spec : sources) {
			struct stat st {};
			if (lstat(spec.open_path.c_str(), &st) != 0)
				fail(format("lstat {}: {}", spec.open_path.string(),
				    std::strerror(errno)));
			scan_path(spec.open_path, opts, spec, st.st_dev,
			    current_slice, slice_num, totals);
		}

		if (!current_slice.entries.empty())
			emit_slice(current_slice, opts, ++slice_num);

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

	} catch (const std::exception &e) {
		fail(e.what());
	}

	return 0;
}
