#include <archive.h>
#include <archive_entry.h>
#include <blake3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
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
	std::cerr << "usage: " << prog << " -f <out-file|-> [-v|-vv] [-x] <path> [path ...]\n";
}

[[noreturn]] void fail_archive(const char *context, archive *a) {
	const char *msg = archive_error_string(a);
	std::cerr << "pax: " << context;
	if (msg != nullptr)
		std::cerr << ": " << msg;
	std::cerr << '\n';
	std::exit(1);
}

[[noreturn]] void fail_errno(const std::string &context) {
	std::cerr << "pax: " << context << ": " << std::strerror(errno) << '\n';
	std::exit(1);
}

[[noreturn]] void fail_error_code(const std::string &context,
    const std::error_code &ec) {
	std::cerr << "pax: " << context << ": " << ec.message() << '\n';
	std::exit(1);
}

void check_archive(int r, archive *a, const char *context) {
	if (r == ARCHIVE_OK)
		return;
	if (r == ARCHIVE_WARN) {
		const char *msg = archive_error_string(a);
		std::cerr << "pax: warning: " << context;
		if (msg != nullptr)
			std::cerr << ": " << msg;
		std::cerr << '\n';
		return;
	}
	fail_archive(context, a);
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

	std::ostringstream hex;
	hex << std::hex << std::setfill('0');
	for (uint8_t byte : output)
		hex << std::setw(2) << static_cast<unsigned>(byte);
	return hex.str();
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
			std::cerr << "pax: unknown option: " << arg << '\n';
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
			std::cerr << "pax: source is not a directory: " << arg << '\n';
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
			std::cerr << "pax: source does not exist: " << arg << '\n';
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
	fs::path child_path = drop_first_component(relative);
	if (!child_path.empty())
		path_in_archive /= child_path;
	return path_in_archive.generic_string();
}

void copy_file_data(archive *writer, archive_entry *entry) {
	const char *source_path = archive_entry_sourcepath(entry);
	if (source_path == nullptr)
		source_path = archive_entry_pathname(entry);
	if (source_path == nullptr)
		fail_archive("entry has no source path", writer);

	int fd = open(source_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		fail_errno(std::string("open ") + source_path);

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
			std::cerr << "pax: short archive write for " << source_path << '\n';
			std::exit(1);
		}
	}

	if (close(fd) != 0)
		fail_errno(std::string("close ") + source_path);
}

void write_source(archive *writer, const Options &opts, const SourceSpec &source) {
	archive *disk = archive_read_disk_new();
	if (disk == nullptr) {
		std::cerr << "pax: cannot allocate disk reader\n";
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
			std::cerr << "pax: cannot allocate archive entry\n";
			std::exit(1);
		}

		int r = archive_read_next_header2(disk, entry);
		if (r == ARCHIVE_EOF) {
			archive_entry_free(entry);
			break;
		}
		check_archive(r, disk, "read filesystem entry");
		check_archive(archive_read_disk_descend(disk), disk, "descend filesystem entry");

		const char *source_path = archive_entry_sourcepath(entry);
		if (source_path != nullptr) {
			std::string archive_path = archive_path_for_source(source, source_path);
			archive_entry_set_pathname(entry, archive_path.c_str());
		}

		if (opts.verbose > 0) {
			std::cerr << "a " << archive_entry_pathname(entry);
			if (opts.verbose > 1) {
				std::cerr << " uid=" << archive_entry_uid(entry)
					  << " gid=" << archive_entry_gid(entry);
				if (archive_entry_uname(entry) != nullptr)
					std::cerr << " uname=" << archive_entry_uname(entry);
				if (archive_entry_gname(entry) != nullptr)
					std::cerr << " gname=" << archive_entry_gname(entry);
			}
			std::cerr << '\n';
		}

		r = archive_write_header(writer, entry);
		if (r < ARCHIVE_OK)
			check_archive(r, writer, "write archive header");
		if (r != ARCHIVE_FATAL && archive_entry_filetype(entry) == AE_IFREG)
			copy_file_data(writer, entry);
		check_archive(archive_write_finish_entry(writer), writer, "finish archive entry");
		archive_entry_free(entry);
	}

	check_archive(archive_read_close(disk), disk, "close disk reader");
	check_archive(archive_read_free(disk), disk, "free disk reader");
}

void write_pax_archive(const Options &opts) {
	archive *writer = archive_write_new();
	if (writer == nullptr) {
		std::cerr << "pax: cannot allocate archive writer\n";
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

	for (const std::string &source_arg : opts.sources)
		write_source(writer, opts, make_source_spec(source_arg));

	check_archive(archive_write_close(writer), writer, "close output");
	check_archive(archive_write_free(writer), writer, "free writer");

	std::cerr << blake3_hex(output.hasher) << "  "
		  << (opts.output == "-" ? "-" : opts.output) << '\n';
}

} // namespace

int main(int argc, char **argv) {
	Options opts = parse_args(argc, argv);
	write_pax_archive(opts);
	return 0;
}
