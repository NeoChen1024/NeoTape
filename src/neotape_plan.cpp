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

struct Options {
	uint64_t slice_size = 64ull * 1024 * 1024 * 1024;
	bool one_file_system = false;
	bool verbose = false;
	std::vector<std::filesystem::path> sources;
};

struct EntryMeta {
	std::filesystem::path path;
	char kind = '?';
	uint64_t disk_bytes = 0;
	uint64_t apparent_bytes = 0;
	dev_t device = 0;
};

struct SlicePlan {
	std::vector<std::size_t> entry_indexes;
	uint64_t disk_bytes = 0;
	uint64_t apparent_bytes = 0;
};

[[noreturn]] void fail(const std::string &message) {
	std::cerr << std::format("neotape-plan: {}\n", message);
	std::exit(1);
}

void warn(const std::string &message) {
	std::cerr << std::format("neotape-plan: warning: {}\n", message);
}

void usage(const char *prog) {
	std::cerr << std::format(
	    "usage: {} [--slice-size <bytes>] [-x] [-v] <path> [path ...]\n", prog);
}

uint64_t parse_size(std::string_view text, const char *name) {
	if (text.empty())
		fail(std::format("{} is empty", name));

	uint64_t multiplier = 1;
	char suffix = text.back();
	if (suffix == 'k' || suffix == 'K' || suffix == 'm' || suffix == 'M' ||
	    suffix == 'g' || suffix == 'G' || suffix == 't' || suffix == 'T') {
		text.remove_suffix(1);
		switch (suffix) {
		case 'k':
		case 'K':
			multiplier = 1024ull;
			break;
		case 'm':
		case 'M':
			multiplier = 1024ull * 1024;
			break;
		case 'g':
		case 'G':
			multiplier = 1024ull * 1024 * 1024;
			break;
		case 't':
		case 'T':
			multiplier = 1024ull * 1024 * 1024 * 1024;
			break;
		}
	}

	std::string owned(text);
	char *end = nullptr;
	errno = 0;
	unsigned long long value = std::strtoull(owned.c_str(), &end, 10);
	if (errno != 0 || end == nullptr || *end != '\0')
		fail(std::format("invalid {}: {}", name, text));
	if (value == 0)
		fail(std::format("{} must be greater than zero", name));
	if (value > UINT64_MAX / multiplier)
		fail(std::format("{} is too large", name));
	return static_cast<uint64_t>(value) * multiplier;
}

Options parse_args(int argc, char **argv) {
	Options opts;
	for (int i = 1; i < argc; ++i) {
		std::string_view arg(argv[i]);
		auto need_value = [&](const char *name) -> std::string {
			if (++i >= argc)
				fail(std::format("{} requires a value", name));
			return argv[i];
		};

		if (arg == "--slice-size") {
			opts.slice_size = parse_size(need_value("--slice-size"), "slice size");
		} else if (arg == "-x") {
			opts.one_file_system = true;
		} else if (arg == "-v") {
			opts.verbose = true;
		} else if (arg == "-h" || arg == "--help") {
			usage(argv[0]);
			std::exit(0);
		} else if (!arg.empty() && arg.front() == '-') {
			fail(std::format("unknown option: {}", arg));
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

std::filesystem::path normalized_path(const std::filesystem::path &path) {
	std::error_code ec;
	std::filesystem::path absolute = std::filesystem::absolute(path, ec);
	if (ec)
		return path;
	return absolute.lexically_normal();
}

void add_entry(const std::filesystem::path &path, const struct stat &st,
    std::vector<EntryMeta> &entries) {
	entries.push_back(EntryMeta{
	    .path = path,
	    .kind = kind_from_mode(st.st_mode),
	    .disk_bytes = disk_bytes_from_stat(st),
	    .apparent_bytes = apparent_bytes_from_stat(st),
	    .device = st.st_dev,
	});
}

std::vector<std::filesystem::path> sorted_children(const std::filesystem::path &path) {
	DIR *dir = opendir(path.c_str());
	if (dir == nullptr) {
		warn(std::format("opendir {}: {}", path.string(), std::strerror(errno)));
		return {};
	}

	std::vector<std::filesystem::path> children;
	for (;;) {
		errno = 0;
		dirent *entry = readdir(dir);
		if (entry == nullptr)
			break;
		std::string_view name(entry->d_name);
		if (name == "." || name == "..")
			continue;
		children.push_back(path / std::string(name));
	}
	if (errno != 0)
		warn(std::format("readdir {}: {}", path.string(), std::strerror(errno)));
	if (closedir(dir) != 0)
		warn(std::format("closedir {}: {}", path.string(), std::strerror(errno)));

	std::ranges::sort(children);
	return children;
}

void scan_path(const std::filesystem::path &path, const Options &opts,
    std::optional<dev_t> root_device, std::vector<EntryMeta> &entries) {
	struct stat st {};
	if (lstat(path.c_str(), &st) != 0) {
		warn(std::format("lstat {}: {}", path.string(), std::strerror(errno)));
		return;
	}

	if (opts.one_file_system && root_device.has_value() && S_ISDIR(st.st_mode) &&
	    st.st_dev != *root_device)
		return;

	add_entry(path, st, entries);
	if (!S_ISDIR(st.st_mode))
		return;

	for (const std::filesystem::path &child : sorted_children(path))
		scan_path(child, opts, root_device, entries);
}

std::vector<EntryMeta> prefetch_metadata(const Options &opts) {
	std::vector<EntryMeta> entries;
	for (const std::filesystem::path &source : opts.sources) {
		std::filesystem::path path = normalized_path(source);
		struct stat st {};
		if (lstat(path.c_str(), &st) != 0)
			fail(std::format("lstat {}: {}", path.string(), std::strerror(errno)));
		scan_path(path, opts, st.st_dev, entries);
	}
	return entries;
}

void add_to_slice(SlicePlan &slice, std::size_t index, const EntryMeta &entry) {
	slice.entry_indexes.push_back(index);
	slice.disk_bytes += entry.disk_bytes;
	slice.apparent_bytes += entry.apparent_bytes;
}

std::vector<SlicePlan> pack_slices(const std::vector<EntryMeta> &entries,
    uint64_t slice_size) {
	std::vector<SlicePlan> slices;
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

std::string human_bytes(uint64_t bytes) {
	const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	double value = static_cast<double>(bytes);
	std::size_t unit = 0;
	while (value >= 1024.0 && unit + 1 < std::size(units)) {
		value /= 1024.0;
		++unit;
	}
	if (unit == 0)
		return std::format("{} B", bytes);
	return std::format("{:.2f} {}", value, units[unit]);
}

void print_plan(const Options &opts, const std::vector<EntryMeta> &entries,
    const std::vector<SlicePlan> &slices) {
	uint64_t total_disk = 0;
	uint64_t total_apparent = 0;
	for (const EntryMeta &entry : entries) {
		total_disk += entry.disk_bytes;
		total_apparent += entry.apparent_bytes;
	}

	std::cout << std::format(
	    "prefetched entries={} total_disk={} total_apparent={} target_slice={}\n",
	    entries.size(), human_bytes(total_disk), human_bytes(total_apparent),
	    human_bytes(opts.slice_size));

	for (std::size_t i = 0; i < slices.size(); ++i) {
		const SlicePlan &slice = slices[i];
		std::cout << std::format("slice {:06}: entries={} disk={} apparent={}\n",
		    i + 1, slice.entry_indexes.size(), human_bytes(slice.disk_bytes),
		    human_bytes(slice.apparent_bytes));
		if (!opts.verbose)
			continue;
		for (std::size_t entry_index : slice.entry_indexes) {
			const EntryMeta &entry = entries[entry_index];
			std::cout << std::format("  {} disk={} apparent={} {}\n", entry.kind,
			    human_bytes(entry.disk_bytes), human_bytes(entry.apparent_bytes),
			    entry.path.generic_string());
		}
	}
}

} // namespace

int main(int argc, char **argv) {
	Options opts = parse_args(argc, argv);
	std::vector<EntryMeta> entries = prefetch_metadata(opts);
	std::vector<SlicePlan> slices = pack_slices(entries, opts.slice_size);
	print_plan(opts, entries, slices);
	return 0;
}
