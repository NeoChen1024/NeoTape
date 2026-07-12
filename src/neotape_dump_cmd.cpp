#include "neotape/format.hpp"
#include "neotape/tape.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using std::format;
using std::string;

namespace fs = std::filesystem;

struct Options {
    string source;
    fs::path target;
    bool verbose = false;
};

[[noreturn]] void fail(const string &message) {
    std::cerr << format("neotape-dump: {}\n", message);
    std::exit(1);
}

[[noreturn]] void usage_error(const string &message) {
    std::cerr << format("neotape-dump: {}\n", message);
    std::exit(2);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --source <tape:/dev/nst0> --target <spool:./dir>\n"
        "       [-v] [-h]\n",
        prog);
}

string strip_locator(const string &value, const string &prefix,
                     const string &option) {
    if (!value.starts_with(prefix) || value.size() == prefix.size()) {
        usage_error(format("{} must use {}<path>", option, prefix));
    }
    return value.substr(prefix.size());
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"target", required_argument, nullptr, 't'},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int option = 0;
    while ((option = getopt_long(argc, argv, "s:t:vh", long_opts, nullptr)) !=
           -1) {
        switch (option) {
        case 's':
            opts.source = strip_locator(optarg, "tape:", "--source");
            break;
        case 't':
            opts.target = strip_locator(optarg, "spool:", "--target");
            break;
        case 'v':
            opts.verbose = true;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        default:
            usage_error(format("unexpected option code {}", option));
        }
    }

    if (opts.source.empty()) {
        usage_error("--source is required");
    }
    if (opts.target.empty()) {
        usage_error("--target is required");
    }
    if (optind != argc) {
        usage_error("unexpected positional arguments");
    }
    return opts;
}

void clear_nonblocking(int fd) {
    int const flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        throw std::runtime_error(format("fcntl: {}", std::strerror(errno)));
    }
}

fs::path dump_path(const fs::path &target, uint64_t file_num) {
    // The dump intentionally does not parse records to invent a semantic
    // slice/archive-end suffix. Spool readers order files by the numeric
    // prefix and accept the optional detail portion opaquely.
    return target / format("neotape-{:06}.dump.nts", file_num);
}

void write_record(std::ofstream &output, const std::byte *data,
                  std::size_t size, const fs::path &path) {
    output.write(reinterpret_cast<const char *>(data),
                 static_cast<std::streamsize>(size));
    if (!output) {
        throw std::runtime_error(format("write {}", path.string()));
    }
}

int do_dump(const Options &opts) {
    if (fs::exists(opts.target) && !fs::is_directory(opts.target)) {
        throw std::runtime_error(
            format("target is not a directory: {}", opts.target.string()));
    }
    fs::create_directories(opts.target);
    if (!fs::is_empty(opts.target)) {
        throw std::runtime_error(
            format("target directory is not empty: {}", opts.target.string()));
    }

    mt::TapeDevice tape(opts.source, false);
    clear_nonblocking(tape.fd());
    tape.rewind();

    std::vector<std::byte> buffer(neotape::max_block_size);
    uint64_t file_num = 0;
    uint64_t records = 0;
    uint64_t bytes = 0;
    fs::path current_path = dump_path(opts.target, file_num);
    std::ofstream output(current_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(format("open {}", current_path.string()));
    }

    for (;;) {
        ssize_t const count = ::read(tape.fd(), buffer.data(), buffer.size());
        if (count > 0) {
            write_record(output, buffer.data(), static_cast<std::size_t>(count),
                         current_path);
            ++records;
            bytes += static_cast<uint64_t>(count);
            continue;
        }

        bool const at_filemark = count == 0 || (count < 0 && errno == EIO);
        if (!at_filemark) {
            throw std::runtime_error(
                format("read {}: {}", opts.source, std::strerror(errno)));
        }

        output.close();
        if (tape.status().eod()) {
            if (fs::file_size(current_path) == 0) {
                fs::remove(current_path);
            }
            break;
        }

        tape.space_fwd_filemark(1);
        if (opts.verbose) {
            std::cerr << format("neotape-dump: wrote {}\n",
                                current_path.string());
        }
        ++file_num;
        current_path = dump_path(opts.target, file_num);
        output.open(current_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(format("open {}", current_path.string()));
        }
    }

    std::cout << format("Dumped {} records ({} bytes) into {} tape files\n",
                        records, bytes, file_num + (records != 0 ? 1 : 0));
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        return do_dump(parse_args(argc, argv));
    } catch (const std::exception &error) {
        fail(error.what());
    }
}
