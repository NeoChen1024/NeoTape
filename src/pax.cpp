#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

struct Options {
	std::string output;
	std::string source_dir;
	int verbose = 0;
	bool one_file_system = false;
};

void usage(const char *prog) {
	std::cerr << "usage: " << prog << " -f <out-file|-> [-v|-vv] [-x] <directory>\n";
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
		} else if (opts.source_dir.empty()) {
			opts.source_dir = std::string(arg);
		} else {
			std::cerr << "pax: only one source directory is supported for now\n";
			usage(argv[0]);
			std::exit(2);
		}
	}

	if (opts.output.empty() || opts.source_dir.empty()) {
		usage(argv[0]);
		std::exit(2);
	}
	return opts;
}

std::string generic_archive_path(const std::filesystem::path &source_parent,
    const char *source_path) {
	std::filesystem::path absolute =
	    std::filesystem::absolute(std::filesystem::path(source_path)).lexically_normal();
	std::filesystem::path relative = absolute.lexically_relative(source_parent);
	if (relative.empty())
		relative = absolute.filename();
	return relative.generic_string();
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

void write_pax_archive(const Options &opts) {
	namespace fs = std::filesystem;

	fs::path source = fs::absolute(opts.source_dir).lexically_normal();
	if (!fs::is_directory(fs::symlink_status(source))) {
		std::cerr << "pax: source is not a directory: " << opts.source_dir << '\n';
		std::exit(1);
	}
	fs::path source_parent = source.parent_path();

	archive *writer = archive_write_new();
	if (writer == nullptr) {
		std::cerr << "pax: cannot allocate archive writer\n";
		std::exit(1);
	}

	check_archive(archive_write_add_filter_none(writer), writer, "set uncompressed output");
	check_archive(archive_write_set_format_pax(writer), writer, "set pax format");
	check_archive(archive_write_set_options(writer, "xattrheader=ALL,hdrcharset=UTF-8"),
	    writer, "set pax options");

	const char *output_name = opts.output == "-" ? nullptr : opts.output.c_str();
	check_archive(archive_write_open_filename(writer, output_name), writer, "open output");

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
	check_archive(archive_read_disk_open(disk, source.c_str()), disk, "open source directory");

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
			std::string archive_path = generic_archive_path(source_parent, source_path);
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
	check_archive(archive_write_close(writer), writer, "close output");
	check_archive(archive_write_free(writer), writer, "free writer");
}

} // namespace

int main(int argc, char **argv) {
	Options opts = parse_args(argc, argv);
	write_pax_archive(opts);
	return 0;
}
