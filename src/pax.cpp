#include <archive.h>
#include <archive_entry.h>
#include <blake3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

struct Options {
	std::string output;
	std::vector<std::string> sources;
	int verbose = 0;
	bool one_file_system = false;
};

struct SourceSpec {
	std::string original;
	std::filesystem::path open_path;
	std::filesystem::path open_parent;
	std::filesystem::path archive_root;
};

struct HashingOutput {
	FILE *file = nullptr;
	bool close_file = false;
	blake3_hasher hasher;
};

void usage(const char *prog) {
	std::cerr << std::format(
	    "usage: {} -f <out-file|-> [-v|-vv] [-x] <path> [path ...]\n", prog);
}

[[noreturn]] void fail_archive(const char *context, archive *a) {
	const char *msg = archive_error_string(a);
	std::cerr << std::format("pax: {}{}\n", context,
	    msg != nullptr ? std::format(": {}", msg) : std::string());
	std::exit(1);
}

[[noreturn]] void fail_errno(const std::string &context) {
	std::cerr << std::format("pax: {}: {}\n", context, std::strerror(errno));
	std::exit(1);
}

[[noreturn]] void fail_error_code(const std::string &context,
    const std::error_code &ec) {
	std::cerr << std::format("pax: {}: {}\n", context, ec.message());
	std::exit(1);
}

void check_archive(int r, archive *a, const char *context) {
	if (r == ARCHIVE_OK)
		return;
	if (r == ARCHIVE_WARN) {
		const char *msg = archive_error_string(a);
		std::cerr << std::format("pax: warning: {}{}\n", context,
		    msg != nullptr ? std::format(": {}", msg) : std::string());
		return;
	}
	fail_archive(context, a);
}

void warn_archive(const char *context, archive *a) {
	const char *msg = archive_error_string(a);
	std::cerr << std::format("pax: warning: {}{}\n", context,
	    msg != nullptr ? std::format(": {}", msg) : std::string());
}

bool locale_name_is_utf8(const char *name) {
	if (name == nullptr)
		return false;

	std::string locale_name(name);
	std::ranges::transform(locale_name, locale_name.begin(),
	    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return locale_name.find("utf-8") != std::string::npos ||
	       locale_name.find("utf8") != std::string::npos;
}

void ensure_utf8_ctype_locale() {
	const char *locale_name = std::setlocale(LC_CTYPE, "");
	if (locale_name_is_utf8(locale_name))
		return;

	for (const char *fallback : {"C.UTF-8", "en_US.UTF-8"}) {
		locale_name = std::setlocale(LC_CTYPE, fallback);
		if (locale_name_is_utf8(locale_name))
			return;
	}
}

int output_open_callback(archive *, void *) {
	return ARCHIVE_OK;
}

la_ssize_t output_write_callback(archive *a, void *client_data, const void *buffer,
    size_t length) {
	auto *out = static_cast<HashingOutput *>(client_data);
	size_t written = fwrite(buffer, 1, length, out->file);
	if (written > 0)
		blake3_hasher_update(&out->hasher, buffer, written);
	if (written != length && ferror(out->file)) {
		archive_set_error(a, errno, "failed to write archive output");
		return -1;
	}
	return static_cast<la_ssize_t>(written);
}

int output_close_callback(archive *a, void *client_data) {
	auto *out = static_cast<HashingOutput *>(client_data);
	if (fflush(out->file) != 0) {
		archive_set_error(a, errno, "failed to flush archive output");
		return ARCHIVE_FATAL;
	}
	if (out->close_file && fclose(out->file) != 0) {
		out->file = nullptr;
		archive_set_error(a, errno, "failed to close archive output");
		return ARCHIVE_FATAL;
	}
	out->file = nullptr;
	return ARCHIVE_OK;
}

std::string blake3_hex(const blake3_hasher &hasher) {
	std::array<uint8_t, BLAKE3_OUT_LEN> output{};
	blake3_hasher_finalize(&hasher, output.data(), output.size());

	std::string hex;
	for (uint8_t byte : output)
		hex += std::format("{:02x}", static_cast<unsigned>(byte));
	return hex;
}

Options parse_args(int argc, char **argv) {
	Options opts;

	for (int i = 1; i < argc; ++i) {
		std::string_view arg(argv[i]);
		if (arg == "-f") {
			if (++i >= argc) {
				usage(argv[0]);
				std::exit(2);
			}
			opts.output = argv[i];
		} else if (arg == "-v") {
			opts.verbose = std::max(opts.verbose, 1);
		} else if (arg == "-vv") {
			opts.verbose = std::max(opts.verbose, 2);
		} else if (arg == "-x") {
			opts.one_file_system = true;
		} else if (arg == "-h" || arg == "--help") {
			usage(argv[0]);
			std::exit(0);
		} else if (!arg.empty() && arg.front() == '-') {
			std::cerr << std::format("pax: unknown option: {}\n", arg);
			usage(argv[0]);
			std::exit(2);
		} else {
			opts.sources.emplace_back(arg);
		}
	}

	if (opts.output.empty() || opts.sources.empty()) {
		usage(argv[0]);
		std::exit(2);
	}
	return opts;
}

bool has_trailing_slash(std::string_view path) {
	return path.size() > 1 && path.back() == '/';
}

std::string strip_trailing_slashes(std::string_view path) {
	while (path.size() > 1 && path.back() == '/')
		path.remove_suffix(1);
	return std::string(path);
}

SourceSpec make_source_spec(const std::string &arg) {
	namespace fs = std::filesystem;

	SourceSpec spec;
	spec.original = arg;

	std::string stripped = strip_trailing_slashes(arg);
	bool follow_top_symlink = has_trailing_slash(arg);
	fs::path display_path = fs::absolute(stripped).lexically_normal();

	std::error_code ec;
	if (follow_top_symlink) {
		fs::file_status status = fs::status(display_path, ec);
		if (ec)
			fail_error_code(std::string("stat ") + arg, ec);
		if (!fs::is_directory(status)) {
			std::cerr << std::format("pax: source is not a directory: {}\n", arg);
			std::exit(1);
		}
		spec.open_path = fs::canonical(display_path, ec);
		if (ec)
			fail_error_code(std::string("canonical ") + arg, ec);
	} else {
		fs::file_status status = fs::symlink_status(display_path, ec);
		if (ec)
			fail_error_code(std::string("lstat ") + arg, ec);
		if (!fs::exists(status)) {
			std::cerr << std::format("pax: source does not exist: {}\n", arg);
			std::exit(1);
		}
		spec.open_path = display_path;
	}

	spec.open_parent = spec.open_path.parent_path();
	spec.archive_root = display_path.filename();
	if (spec.archive_root.empty())
		spec.archive_root = ".";
	return spec;
}

std::filesystem::path drop_first_component(const std::filesystem::path &path) {
	std::filesystem::path out;
	bool first = true;
	for (const auto &component : path) {
		if (first) {
			first = false;
			continue;
		}
		out /= component;
	}
	return out;
}

std::string archive_path_for_source(const SourceSpec &spec, const char *source_path) {
	namespace fs = std::filesystem;

	fs::path absolute = fs::absolute(fs::path(source_path)).lexically_normal();
	fs::path relative = absolute.lexically_relative(spec.open_parent);
	fs::path path_in_archive = spec.archive_root;
	fs::path child_path =
	    spec.archive_root == "." ? relative : drop_first_component(relative);
	if (!child_path.empty())
		path_in_archive /= child_path;
	return path_in_archive.generic_string();
}

void mark_link_target_as_utf8(archive_entry *entry) {
	if (const char *symlink = archive_entry_symlink(entry); symlink != nullptr)
		archive_entry_update_symlink_utf8(entry, symlink);
	else if (const char *hardlink = archive_entry_hardlink(entry); hardlink != nullptr)
		archive_entry_update_hardlink_utf8(entry, hardlink);
}

std::string entry_owner_name(archive_entry *entry) {
	if (const char *name = archive_entry_uname(entry); name != nullptr)
		return name;
	return std::to_string(archive_entry_uid(entry));
}

std::string entry_group_name(archive_entry *entry) {
	if (const char *name = archive_entry_gname(entry); name != nullptr)
		return name;
	return std::to_string(archive_entry_gid(entry));
}

std::string entry_timestamp(archive_entry *entry) {
	std::time_t mtime = archive_entry_mtime(entry);
	std::tm local_time{};
	if (localtime_r(&mtime, &local_time) == nullptr)
		return "00000000T000000+0000";

	char buffer[32]{};
	if (std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%S%z", &local_time) == 0)
		return "00000000T000000+0000";
	return buffer;
}

std::string entry_display_path(archive_entry *entry) {
	std::string path = archive_entry_pathname(entry) != nullptr
	    ? archive_entry_pathname(entry)
	    : "";
	if (archive_entry_filetype(entry) == AE_IFDIR && !path.empty() && path.back() != '/')
		path += '/';
	return path;
}

std::string verbose_line(archive_entry *entry) {
	std::string mode = archive_entry_strmode(entry);
	if (mode.size() > 10)
		mode.resize(10);
	if (archive_entry_hardlink(entry) != nullptr && !mode.empty())
		mode[0] = 'h';
	return std::format("{} {:3} {:>10} {:>10} {:6} [{}] {}", mode,
	    archive_entry_nlink(entry), entry_owner_name(entry), entry_group_name(entry),
	    archive_entry_size(entry), entry_timestamp(entry), entry_display_path(entry));
}

int open_entry_file(archive_entry *entry) {
	const char *source_path = archive_entry_sourcepath(entry);
	if (source_path == nullptr)
		source_path = archive_entry_pathname(entry);
	if (source_path == nullptr)
		return -1;

	int fd = open(source_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		std::cerr << std::format("pax: warning: open {}: {}\n", source_path,
		    std::strerror(errno));
		return -1;
	}
	return fd;
}

void copy_file_data(archive *writer, archive_entry *entry, int fd) {
	const char *source_path = archive_entry_sourcepath(entry);
	if (source_path == nullptr)
		source_path = archive_entry_pathname(entry);
	if (source_path == nullptr)
		fail_archive("entry has no source path", writer);

	std::vector<char> buffer(1024 * 1024);
	for (;;) {
		ssize_t n = read(fd, buffer.data(), buffer.size());
		if (n < 0) {
			int saved_errno = errno;
			close(fd);
			errno = saved_errno;
			fail_errno(std::string("read ") + source_path);
		}
		if (n == 0)
			break;
		ssize_t written = archive_write_data(writer, buffer.data(), static_cast<size_t>(n));
		if (written < 0) {
			close(fd);
			fail_archive("write file data", writer);
		}
		if (written != n) {
			close(fd);
			std::cerr << std::format("pax: short archive write for {}\n", source_path);
			std::exit(1);
		}
	}

	if (close(fd) != 0) {
		std::cerr << std::format("pax: warning: close {}: {}\n", source_path,
		    std::strerror(errno));
	}
}

bool write_archive_entry(archive *writer, const Options &opts, archive_entry *entry) {
	mark_link_target_as_utf8(entry);

	if (opts.verbose > 1)
		std::cerr << std::format("{}\n", verbose_line(entry));
	else if (opts.verbose > 0)
		std::cerr << std::format("a {}\n", entry_display_path(entry));

	int fd = -1;
	if (archive_entry_filetype(entry) == AE_IFREG && archive_entry_size(entry) > 0) {
		fd = open_entry_file(entry);
		if (fd < 0)
			return false;
	}

	int r = archive_write_header(writer, entry);
	if (r == ARCHIVE_FATAL) {
		if (fd >= 0)
			close(fd);
		fail_archive("write archive header", writer);
	}
	if (r < ARCHIVE_OK) {
		if (fd >= 0)
			close(fd);
		warn_archive("write archive header", writer);
		return false;
	}
	if (fd >= 0)
		copy_file_data(writer, entry, fd);
	check_archive(archive_write_finish_entry(writer), writer, "finish archive entry");
	return true;
}

void write_linkified_entry(archive *writer, const Options &opts, archive_entry *entry) {
	if (entry == nullptr)
		return;
	write_archive_entry(writer, opts, entry);
	archive_entry_free(entry);
}

void flush_hardlink_resolver(archive *writer, const Options &opts,
    archive_entry_linkresolver *resolver) {
	for (;;) {
		archive_entry *entry = nullptr;
		archive_entry *spare = nullptr;
		archive_entry_linkify(resolver, &entry, &spare);
		if (entry == nullptr && spare == nullptr)
			break;
		write_linkified_entry(writer, opts, entry);
		write_linkified_entry(writer, opts, spare);
	}
}

void write_source(archive *writer, const Options &opts, const SourceSpec &source,
    archive_entry_linkresolver *resolver) {
	archive *disk = archive_read_disk_new();
	if (disk == nullptr) {
		std::cerr << std::format("pax: cannot allocate disk reader\n");
		std::exit(1);
	}
	check_archive(archive_read_disk_set_symlink_physical(disk), disk,
	    "set physical symlink mode");
	if (opts.one_file_system) {
		check_archive(archive_read_disk_set_behavior(disk,
				  ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS),
		    disk, "set one-file-system behavior");
	}
	check_archive(archive_read_disk_set_standard_lookup(disk), disk,
	    "set uid/gid name lookup");
	check_archive(archive_read_disk_open(disk, source.open_path.c_str()), disk,
	    "open source path");

	for (;;) {
		archive_entry *entry = archive_entry_new();
		if (entry == nullptr) {
			std::cerr << std::format("pax: cannot allocate archive entry\n");
			std::exit(1);
		}

		int r = archive_read_next_header2(disk, entry);
		if (r == ARCHIVE_EOF) {
			archive_entry_free(entry);
			break;
		}
		if (r == ARCHIVE_FATAL)
			fail_archive("read filesystem entry", disk);
		if (r < ARCHIVE_OK) {
			warn_archive("read filesystem entry", disk);
			archive_entry_free(entry);
			continue;
		}
		if (archive_read_disk_can_descend(disk)) {
			r = archive_read_disk_descend(disk);
			if (r == ARCHIVE_FATAL)
				fail_archive("descend filesystem entry", disk);
			if (r < ARCHIVE_OK)
				warn_archive("descend filesystem entry", disk);
		}

		const char *source_path = archive_entry_sourcepath(entry);
		if (source_path != nullptr) {
			std::string archive_path = archive_path_for_source(source, source_path);
			archive_entry_set_pathname_utf8(entry, archive_path.c_str());
		}

		archive_entry *spare = nullptr;
		archive_entry_linkify(resolver, &entry, &spare);
		write_linkified_entry(writer, opts, entry);
		write_linkified_entry(writer, opts, spare);
	}

	check_archive(archive_read_close(disk), disk, "close disk reader");
	check_archive(archive_read_free(disk), disk, "free disk reader");
}

void write_pax_archive(const Options &opts) {
	archive *writer = archive_write_new();
	if (writer == nullptr) {
		std::cerr << std::format("pax: cannot allocate archive writer\n");
		std::exit(1);
	}

	check_archive(archive_write_add_filter_none(writer), writer, "set uncompressed output");
	check_archive(archive_write_set_format_pax(writer), writer, "set pax format");
	check_archive(archive_write_set_options(writer, "xattrheader=ALL,hdrcharset=UTF-8"),
	    writer, "set pax options");

	HashingOutput output;
	blake3_hasher_init(&output.hasher);
	if (opts.output == "-") {
		output.file = stdout;
		output.close_file = false;
	} else {
		output.file = fopen(opts.output.c_str(), "wb");
		if (output.file == nullptr)
			fail_errno(std::string("open ") + opts.output);
		output.close_file = true;
	}
	check_archive(archive_write_open(writer, &output, output_open_callback,
			  output_write_callback, output_close_callback),
	    writer, "open output");

	archive_entry_linkresolver *resolver = archive_entry_linkresolver_new();
	if (resolver == nullptr) {
		std::cerr << std::format("pax: cannot allocate hardlink resolver\n");
		std::exit(1);
	}
	archive_entry_linkresolver_set_strategy(resolver, archive_format(writer));

	for (const std::string &source_arg : opts.sources)
		write_source(writer, opts, make_source_spec(source_arg), resolver);
	flush_hardlink_resolver(writer, opts, resolver);
	archive_entry_linkresolver_free(resolver);

	check_archive(archive_write_close(writer), writer, "close output");
	check_archive(archive_write_free(writer), writer, "free writer");

	std::cerr << std::format("{}  {}\n", blake3_hex(output.hasher),
	    opts.output == "-" ? "-" : opts.output);
}

} // namespace

int main(int argc, char **argv) {
	ensure_utf8_ctype_locale();
	Options opts = parse_args(argc, argv);
	write_pax_archive(opts);
	return 0;
}
