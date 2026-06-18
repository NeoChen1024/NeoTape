#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tape.hpp"
#include "neotape/tcp_protocol.hpp"

#include <cerrno>
#include <csignal>
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

using neotape::connect_to_server;
using neotape::FdGuard;
using neotape::tcp::Message;
using neotape::tcp::MessageType;
using std::format;
using std::string;
using std::vector;

struct SourceLocator {
    enum Kind { none, tape, spool } kind = none;
    std::string path;
};

SourceLocator parse_source(const std::string &s) {
    if (s.starts_with("tape:")) {
        return {SourceLocator::tape, s.substr(5)};
    }
    if (s.starts_with("spool:")) {
        return {SourceLocator::spool, s.substr(6)};
    }
    throw std::runtime_error("source must be tape:<device> or spool:<dir>");
}

struct Options {
    SourceLocator source;
    string connect_address;
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
                        "       --connect <tcp://host:port|unix://path>\n",
                        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"connect", required_argument, nullptr, 'c'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "s:c:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = parse_source(optarg);
            break;
        case 'c':
            opts.connect_address = optarg;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.source.kind == SourceLocator::none) {
        usage_error("--source is required");
    }
    if (opts.connect_address.empty()) {
        usage_error("--connect is required");
    }

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
        ssize_t const n = ::read(dev_->fd(), buffer_.data(), buffer_.size());
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

        if (!fill(neotape::fixed_header_size, filemark_count, eod)) {
            return std::nullopt;
        }

        neotape::FrameHeader const header = neotape::parse_fixed_header(
            reinterpret_cast<const uint8_t *>(pending_.data()),
            neotape::fixed_header_size);

        size_t const record_size = neotape::decoded_block_size(header);

        if (!fill(record_size, filemark_count, eod)) {
            throw std::runtime_error("truncated record in spool source");
        }

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
            ssize_t const n = ::read(dev_->fd(), tmp, sizeof(tmp));
            if (n < 0) {
                if (errno == EIO) {
                    if (!pending_.empty()) {
                        throw std::runtime_error(
                            "EIO in middle of spool record");
                    }
                    advance_filemark(filemark_count);
                    continue;
                }
                throw std::runtime_error(
                    format("read: {}", std::strerror(errno)));
            }
            if (n == 0) {
                if (!pending_.empty()) {
                    throw std::runtime_error(
                        "truncated record at spool file boundary");
                }
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
    std::signal(SIGPIPE, SIG_IGN);

    try {
        Options opts = parse_args(argc, argv);

        std::unique_ptr<mt::TapeDevice> source_dev;
        bool source_is_tape = false;
        if (opts.source.kind == SourceLocator::tape) {
            source_dev =
                std::make_unique<mt::TapeDevice>(opts.source.path, false);
            int const flags = ::fcntl(source_dev->fd(), F_GETFL, 0);
            if (flags >= 0) {
                ::fcntl(source_dev->fd(), F_SETFL, flags & ~O_NONBLOCK);
            }
            source_dev->rewind();
            source_is_tape = true;
        } else {
            if (!std::filesystem::exists(opts.source.path)) {
                fail(format("source spool directory does not exist: {}",
                            opts.source.path));
            }
            source_dev = std::make_unique<mt::SpoolTapeDevice>(
                std::filesystem::path(opts.source.path), false);
        }

        std::unique_ptr<SourceReader> reader;
        if (source_is_tape) {
            reader = std::make_unique<TapeSourceReader>(source_dev.get());
        } else {
            reader = std::make_unique<SpoolSourceReader>(
                dynamic_cast<mt::SpoolTapeDevice *>(source_dev.get()));
        }

        FdGuard const fd_guard(connect_to_server(opts.connect_address));
        int const fd = fd_guard.fd;

        uint64_t record_count = 0;
        uint64_t filemark_count = 0;

        for (;;) {
            auto msg = neotape::tcp::read_message(fd);
            if (!msg) {
                break;
            }

            switch (msg->type) {
            case MessageType::next_frame: {
                bool eod = false;
                auto record = reader->next_record(filemark_count, eod);

                while (!eod && !record) {
                    record = reader->next_record(filemark_count, eod);
                }

                if (eod) {
                    neotape::tcp::write_message(
                        fd, Message{MessageType::tape_eof, {}});
                    std::cerr << format("neotape-read: {} frames sent\n",
                                        record_count);
                    return 0;
                }

                {
                    vector<std::byte> frame_bytes;
                    frame_bytes.swap(*record);
                    Message frame_msg;
                    frame_msg.type = MessageType::frame_record;
                    frame_msg.payload.swap(frame_bytes);
                    neotape::tcp::write_message(fd, frame_msg);
                }
                ++record_count;

                auto ack = neotape::tcp::read_message(fd);
                if (!ack) {
                    fail("unexpected disconnect after frame_record");
                }
                if (ack->type != MessageType::ack_frame) {
                    fail(format("expected ack_frame, got message type {}",
                                static_cast<int>(ack->type)));
                }
                break;
            }
            case MessageType::error: {
                string reason;
                reason.reserve(msg->payload.size());
                for (std::byte const b : msg->payload) {
                    reason.push_back(static_cast<char>(b));
                }
                if (reason.empty()) {
                    reason = "extractor reported error";
                }
                fail(reason);
            }
            default:
                fail(format("unexpected message type {}",
                            static_cast<int>(msg->type)));
            }
        }

        std::cerr << format("neotape-read: {} frames sent\n", record_count);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
