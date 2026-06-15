#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/tape.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <optional>
#include <unistd.h>
#include <vector>

namespace {

using std::format;
using std::string;
using std::vector;

struct SourceLocator {
    enum Kind { none, tape, spool } kind = none;
    std::string path;
};

struct TargetLocator {
    enum Kind { none, spool } kind = none;
    std::string path;
};

SourceLocator parse_source(const std::string &s) {
    if (s.rfind("tape:", 0) == 0)
        return {SourceLocator::tape, s.substr(5)};
    if (s.rfind("spool:", 0) == 0)
        return {SourceLocator::spool, s.substr(6)};
    throw std::runtime_error("source must be tape:<device> or spool:<dir>");
}

TargetLocator parse_target(const std::string &s) {
    if (s.rfind("spool:", 0) == 0)
        return {TargetLocator::spool, s.substr(6)};
    throw std::runtime_error("target must be spool:<dir>");
}

struct Options {
    SourceLocator source;
    TargetLocator target;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-read: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-read: {}\n", msg);
    std::exit(2);
}

void usage(const char *prog) {
    std::cerr << format("usage: {} --source <tape:/dev/nst0|spool:./dir>\n"
                        "       --target <spool:./out>\n",
                        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"target", required_argument, nullptr, 't'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "s:t:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = parse_source(optarg);
            break;
        case 't':
            opts.target = parse_target(optarg);
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.source.kind == SourceLocator::none)
        usage_error("--source is required");
    if (opts.target.kind == TargetLocator::none)
        usage_error("--target is required");

    return opts;
}

class SourceReader {
  public:
    virtual ~SourceReader() = default;
    virtual std::optional<vector<std::byte>>
    next_record(uint64_t &filemark_count, bool &eod) = 0;
};

class TapeSourceReader final : public SourceReader {
  public:
    explicit TapeSourceReader(mt::TapeDevice *dev)
        : dev_(dev), buffer_(neotape::max_block_size) {}

    std::optional<vector<std::byte>> next_record(uint64_t &filemark_count,
                                                 bool &eod) override {
        eod = false;
        ssize_t n = ::read(dev_->fd(), buffer_.data(), buffer_.size());
        if (n < 0) {
            if (errno == EIO) {
                dev_->space_fwd_filemark(1);
                ++filemark_count;
                return std::nullopt;
            }
            throw std::runtime_error(format("read: {}", std::strerror(errno)));
        }
        if (n == 0) {
            if (dev_->status().eod()) {
                eod = true;
                return std::nullopt;
            }
            ++filemark_count;
            return std::nullopt;
        }
        return vector<std::byte>(buffer_.data(), buffer_.data() + n);
    }

  private:
    mt::TapeDevice *dev_;
    vector<std::byte> buffer_;
};

class SpoolSourceReader final : public SourceReader {
  public:
    explicit SpoolSourceReader(mt::SpoolTapeDevice *dev) : dev_(dev) {}

    std::optional<vector<std::byte>> next_record(uint64_t &filemark_count,
                                                 bool &eod) override {
        eod = false;

        if (!fill(neotape::fixed_header_size, filemark_count, eod))
            return std::nullopt;

        neotape::FrameHeader header = neotape::parse_fixed_header(
            reinterpret_cast<const uint8_t *>(pending_.data()),
            neotape::fixed_header_size);

        size_t record_size = neotape::decoded_block_size(header);

        if (!fill(record_size, filemark_count, eod))
            throw std::runtime_error("truncated record in spool source");

        auto off = static_cast<std::ptrdiff_t>(record_size);
        vector<std::byte> record(pending_.begin(), pending_.begin() + off);
        pending_.erase(pending_.begin(), pending_.begin() + off);
        return record;
    }

  private:
    mt::SpoolTapeDevice *dev_;
    vector<std::byte> pending_;

    void advance_filemark(uint64_t &filemark_count) {
        dev_->space_fwd_filemark(1);
        ++filemark_count;
    }

    bool fill(size_t min_bytes, uint64_t &filemark_count, bool &eod) {
        eod = false;
        while (pending_.size() < min_bytes) {
            if (dev_->fd() < 0) {
                eod = true;
                return pending_.size() >= min_bytes;
            }

            std::byte tmp[65536];
            ssize_t n = ::read(dev_->fd(), tmp, sizeof(tmp));
            if (n < 0) {
                if (errno == EIO) {
                    if (!pending_.empty())
                        throw std::runtime_error(
                            "EIO in middle of spool record");
                    advance_filemark(filemark_count);
                    continue;
                }
                throw std::runtime_error(
                    format("read: {}", std::strerror(errno)));
            }
            if (n == 0) {
                if (!pending_.empty())
                    throw std::runtime_error(
                        "truncated record at spool file boundary");
                advance_filemark(filemark_count);
                continue;
            }
            pending_.insert(pending_.end(), tmp, tmp + n);
        }
        return true;
    }
};

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);

        std::unique_ptr<mt::TapeDevice> source_dev;
        bool source_is_tape = false;
        if (opts.source.kind == SourceLocator::tape) {
            auto dev =
                std::make_unique<mt::TapeDevice>(opts.source.path, false);
            // Tape devices are opened non-blocking; reads are blocking.
            int flags = ::fcntl(dev->fd(), F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(dev->fd(), F_SETFL, flags & ~O_NONBLOCK);
            dev->rewind();
            source_dev = std::move(dev);
            source_is_tape = true;
        } else {
            if (!std::filesystem::exists(opts.source.path))
                fail(format("source spool directory does not exist: {}",
                            opts.source.path));
            source_dev = std::make_unique<mt::SpoolTapeDevice>(
                std::filesystem::path(opts.source.path), false);
        }

        mt::SpoolTapeDevice target_dev(std::filesystem::path(opts.target.path),
                                       true);

        std::unique_ptr<SourceReader> reader;
        if (source_is_tape) {
            reader = std::make_unique<TapeSourceReader>(source_dev.get());
        } else {
            reader = std::make_unique<SpoolSourceReader>(
                dynamic_cast<mt::SpoolTapeDevice *>(source_dev.get()));
        }

        uint64_t record_count = 0;
        uint64_t filemark_count = 0;
        bool has_content_or_metadata_frame = false;
        bool has_archive_end_frame = false;

        for (;;) {
            bool eod = false;
            auto record = reader->next_record(filemark_count, eod);
            if (eod)
                break;
            if (!record)
                continue;

            target_dev.write_record(record->data(), record->size());

            if (record->size() >= neotape::fixed_header_size) {
                try {
                    neotape::FrameHeader header = neotape::parse_fixed_header(
                        reinterpret_cast<const uint8_t *>(record->data()),
                        record->size());
                    if (header.channel_type ==
                            neotape::ChannelType::CH_CONTENT ||
                        header.channel_type ==
                            neotape::ChannelType::CH_METADATA)
                        has_content_or_metadata_frame = true;
                    if (header.channel_type ==
                        neotape::ChannelType::ARCHIVE_END)
                        has_archive_end_frame = true;
                } catch (const std::exception &) {
                    // Not a parseable NeoTape header.
                }
            }

            ++record_count;
        }

        std::cerr << format("neotape-read: {} records, {} filemarks\n",
                            record_count, filemark_count);
        std::cerr << format("  content/metadata frame: {}\n",
                            has_content_or_metadata_frame ? "yes" : "no");
        std::cerr << format("  archive end frame: {}\n",
                            has_archive_end_frame ? "yes" : "no");

        if (!has_content_or_metadata_frame || !has_archive_end_frame) {
            if (!has_content_or_metadata_frame)
                fail("did not see content or metadata frame");
            fail("did not see archive end frame");
        }

        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
