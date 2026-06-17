#include "neotape/closable_queue.hpp"
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/tcp_protocol.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using neotape::tcp::Address;
using neotape::tcp::Message;
using neotape::tcp::MessageType;
using neotape::tcp::parse_address;
using std::format;
using std::string;

struct Options {
    string listen_address;
    string input_name = "-";
    uint32_t volume_block_size = 4 * 1024 * 1024;
    string archive_name = "raw";
    uint64_t retention_frame_count = 256;
    bool debug = false;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-raw-store: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-raw-store: {}\n", msg);
    std::exit(2);
}

[[noreturn]] void fail_errno(const string &context) {
    fail(format("{}: {}", context, std::strerror(errno)));
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --listen <tcp://host:port|unix://path>\n"
        "       [--input <file|->] [--volume-block-size <bytes>]\n"
        "       [--archive-name <name>] [--retention-frame-count <N>]\n"
        "       [--debug] [-h]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"input", required_argument, nullptr, 'i'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"archive-name", required_argument, nullptr, 'n'},
        {"retention-frame-count", required_argument, nullptr, 256},
        {"debug", no_argument, nullptr, 257},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "l:i:b:n:h", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 'l':
            opts.listen_address = optarg;
            break;
        case 'i':
            opts.input_name = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 256: {
            char *end = nullptr;
            unsigned long const n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n == 0 || n > 1000000) {
                std::cerr << "neotape-raw-store: --retention-frame-count "
                             "requires a number from 1 to 1000000\n";
                std::exit(2);
            }
            opts.retention_frame_count = static_cast<uint64_t>(n);
            break;
        }
        case 257:
            opts.debug = true;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        default:
            usage_error(format("unexpected option code {}", c));
        }
    }

    if (opts.listen_address.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (optind < argc) {
        usage_error("unexpected positional arguments");
    }
    if (!neotape::valid_block_size(opts.volume_block_size)) {
        usage_error("invalid volume block size");
    }

    return opts;
}

struct FileGuard {
    FILE *file = nullptr;
    bool owned = false;
    FileGuard(FILE *f, bool own) : file(f), owned(own) {}
    ~FileGuard() {
        if (owned && file != nullptr) {
            std::fclose(file);
        }
    }
    FileGuard(const FileGuard &) = delete;
    FileGuard &operator=(const FileGuard &) = delete;
    FileGuard(FileGuard &&) = delete;
    FileGuard &operator=(FileGuard &&) = delete;
};

struct FdGuard {
    int fd = -1;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
    FdGuard(FdGuard &&) = delete;
    FdGuard &operator=(FdGuard &&) = delete;
};

struct ThreadJoiner {
    std::thread *thread = nullptr;
    explicit ThreadJoiner(std::thread &t) : thread(&t) {}
    ~ThreadJoiner() {
        if (thread != nullptr && thread->joinable()) {
            thread->join();
        }
    }
    ThreadJoiner(const ThreadJoiner &) = delete;
    ThreadJoiner &operator=(const ThreadJoiner &) = delete;
    ThreadJoiner(ThreadJoiner &&) = delete;
    ThreadJoiner &operator=(ThreadJoiner &&) = delete;
};

int create_listener(const string &addr) {
    Address a = parse_address(addr);

    int fd = -1;
    if (a.is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error(
                format("socket: {}", std::strerror(errno)));
        }
        unlink(a.path.c_str());
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (a.path.size() >= sizeof(sa.sun_path)) {
            throw std::runtime_error("unix socket path too long");
        }
        std::memcpy(sa.sun_path, a.path.data(), a.path.size());
        if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0) {
            throw std::runtime_error(
                format("bind {}: {}", a.path, std::strerror(errno)));
        }
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *res = nullptr;
        int const gai =
            getaddrinfo(a.host.c_str(), a.port.c_str(), &hints, &res);
        if (gai != 0) {
            throw std::runtime_error(
                format("getaddrinfo: {}", gai_strerror(gai)));
        }
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> const res_guard(
            res, freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            throw std::runtime_error(
                format("socket: {}", std::strerror(errno)));
        }
        int yes = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
            throw std::runtime_error(
                format("bind {}:{}: {}", a.host, a.port, std::strerror(errno)));
        }
    }

    if (listen(fd, 1) < 0) {
        throw std::runtime_error(format("listen: {}", std::strerror(errno)));
    }
    return fd;
}

void copy_header_to_record(const neotape::HeaderBytes &header,
                           std::vector<std::byte> &record) {
    for (std::size_t i = 0; i < header.size(); ++i) {
        record[i] = static_cast<std::byte>(header[i]);
    }
}

void finalize_record_hash(neotape::FrameHeader &header,
                          std::vector<std::byte> &record) {
    neotape::HeaderBytes header_bytes = neotape::serialize_frame_header(header);
    copy_header_to_record(header_bytes, record);
    header.frame_hash = neotape::compute_frame_hash(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
    header_bytes = neotape::serialize_frame_header(header);
    copy_header_to_record(header_bytes, record);
}

void patch_volume_seq_num(std::vector<std::byte> &record,
                          uint64_t new_volume_seq_num) {
    neotape::FrameHeader hdr = neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
    if (hdr.volume_seq_num == new_volume_seq_num) {
        return;
    }
    hdr.volume_seq_num = new_volume_seq_num;
    finalize_record_hash(hdr, record);
}

struct BuiltFrame {
    std::vector<std::byte> record;
    uint64_t global_seq_num = 0;
};

class RawFrameBuilder {
  public:
    RawFrameBuilder(uint32_t block_size, string archive_uuid,
                    string archive_name)
        : block_size_(block_size), archive_uuid_(std::move(archive_uuid)),
          archive_name_(std::move(archive_name)) {}

    [[nodiscard]] uint32_t payload_capacity() const {
        return block_size_ - neotape::fixed_header_size;
    }

    std::vector<BuiltFrame> feed(std::span<const std::byte> bytes) {
        pending_.insert(pending_.end(), bytes.begin(), bytes.end());

        std::vector<BuiltFrame> out;
        const uint32_t cap = payload_capacity();
        while (pending_.size() > cap) {
            out.push_back(build_content_frame(
                std::span<const std::byte>(pending_.data(), cap), false));
            pending_.erase(pending_.begin(), pending_.begin() + cap);
        }
        return out;
    }

    BuiltFrame finish() {
        BuiltFrame frame = build_content_frame(
            std::span<const std::byte>(pending_.data(), pending_.size()), true);
        pending_.clear();
        return frame;
    }

    [[nodiscard]] uint64_t last_global_seq_num() const {
        return global_frame_seq_num_ == 1 ? 0 : global_frame_seq_num_ - 1;
    }

  private:
    BuiltFrame build_content_frame(std::span<const std::byte> payload,
                                   bool is_final) {
        assert(payload.size() <= payload_capacity());

        uint64_t flags = 0;
        if (frame_seq_within_channel_ == 1) {
            flags |= neotape::frame_flag_start;
        }
        if (is_final) {
            flags |= neotape::frame_flag_end;
        }

        neotape::FrameHeader fh;
        fh.channel_type = neotape::ChannelType::CH_CONTENT;
        fh.volume_block_size_kib = static_cast<uint16_t>(block_size_ / 1024U);
        fh.archive_uuid = archive_uuid_;
        fh.archive_label = archive_name_;
        fh.volume_seq_num = 0;
        fh.global_frame_seq_num = global_frame_seq_num_++;
        fh.logical_slice_seq_num = 1;
        fh.frame_seq_num_within_channel = frame_seq_within_channel_++;
        fh.frame_payload_size = static_cast<uint32_t>(payload.size());
        fh.flags = flags;

        std::vector<std::byte> record(block_size_, std::byte{0});
        neotape::HeaderBytes const header = neotape::serialize_frame_header(fh);
        copy_header_to_record(header, record);
        std::copy(payload.begin(), payload.end(),
                  record.begin() +
                      static_cast<std::ptrdiff_t>(neotape::fixed_header_size));
        finalize_record_hash(fh, record);
        return BuiltFrame{std::move(record), fh.global_frame_seq_num};
    }

    uint32_t block_size_;
    string archive_uuid_;
    string archive_name_;
    uint64_t global_frame_seq_num_ = 1;
    uint64_t frame_seq_within_channel_ = 1;
    std::vector<std::byte> pending_;
};

std::vector<std::byte> build_archive_end_record(uint32_t block_size,
                                                uint64_t volume_seq_num,
                                                const string &archive_uuid,
                                                const string &archive_name,
                                                uint64_t global_seq_num) {
    neotape::FrameHeader h;
    h.channel_type = neotape::ChannelType::ARCHIVE_END;
    h.volume_block_size_kib = static_cast<uint16_t>(block_size / 1024U);
    h.archive_uuid = archive_uuid;
    h.archive_label = archive_name;
    h.volume_seq_num = volume_seq_num;
    h.global_frame_seq_num = global_seq_num;
    h.logical_slice_seq_num = 0;
    h.frame_seq_num_within_channel = 1;
    h.frame_payload_size = 0;
    h.flags = neotape::frame_flag_start | neotape::frame_flag_end |
              neotape::frame_flag_clean_end;

    std::vector<std::byte> record(block_size, std::byte{0});
    finalize_record_hash(h, record);
    return record;
}

struct RecordOrDone {
    std::vector<std::byte> record;
    uint64_t global_seq_num = 0;
    bool tape_eof = false;
    bool done = false;
};

struct RetainedFrame {
    uint64_t global_seq_num = 0;
    std::vector<std::byte> record;
};

class FrameRetentionBuffer {
  public:
    explicit FrameRetentionBuffer(size_t max_frames)
        : max_frames_(max_frames) {}

    void add(uint64_t global_seq_num, std::vector<std::byte> record) {
        if (frames_.size() == max_frames_) {
            frames_.pop_front();
        }
        frames_.push_back(RetainedFrame{global_seq_num, std::move(record)});
    }

    void ack(uint64_t global_seq_num) {
        for (auto it = frames_.begin(); it != frames_.end(); ++it) {
            if (it->global_seq_num == global_seq_num) {
                frames_.erase(it);
                return;
            }
        }
    }

    [[nodiscard]] const std::vector<std::byte> *
    get(uint64_t global_seq_num) const {
        for (const auto &f : frames_) {
            if (f.global_seq_num == global_seq_num) {
                return &f.record;
            }
        }
        return nullptr;
    }

  private:
    size_t max_frames_;
    std::deque<RetainedFrame> frames_;
};

void send_error(int client, const char *text) {
    auto payload = std::vector<std::byte>(
        reinterpret_cast<const std::byte *>(text),
        reinterpret_cast<const std::byte *>(text) + std::strlen(text));
    neotape::tcp::write_message(
        client, Message{MessageType::error, std::move(payload)});
}

uint64_t le64_from_bytes(const std::vector<std::byte> &payload) {
    uint64_t v = 0;
    for (std::size_t i = 0; i < payload.size() && i < 8; ++i) {
        v |= static_cast<uint64_t>(static_cast<uint8_t>(payload[i])) << (8 * i);
    }
    return v;
}

void produce_raw_frames(FILE *input, RawFrameBuilder &builder,
                        neotape::ClosableQueue<RecordOrDone> &queue) {
    std::vector<std::byte> buf(1024ULL * 1024ULL);
    for (;;) {
        size_t const n = std::fread(buf.data(), 1, buf.size(), input);
        if (n > 0) {
            auto frames =
                builder.feed(std::span<const std::byte>(buf.data(), n));
            for (auto &frame : frames) {
                if (!queue.push(RecordOrDone{std::move(frame.record),
                                             frame.global_seq_num, false,
                                             false})) {
                    throw std::runtime_error("frame consumer disconnected");
                }
            }
        }
        if (n < buf.size()) {
            if (std::ferror(input) != 0) {
                throw std::runtime_error(
                    format("read input: {}", std::strerror(errno)));
            }
            break;
        }
    }

    BuiltFrame final_frame = builder.finish();
    if (!queue.push(RecordOrDone{std::move(final_frame.record),
                                 final_frame.global_seq_num, false, false})) {
        throw std::runtime_error("frame consumer disconnected");
    }
    if (!queue.push(RecordOrDone{{}, 0, true, false})) {
        throw std::runtime_error("frame consumer disconnected");
    }
    if (!queue.push(
            RecordOrDone{{}, builder.last_global_seq_num(), false, true})) {
        throw std::runtime_error("frame consumer disconnected");
    }
}

struct ServeResult {
    bool archive_complete = false;
    bool volume_committed = false;
    uint64_t frames_served = 0;
};

struct RawStoreState {
    uint64_t next_volume_seq_num = 1;
    uint64_t last_acked_global_frame = 0;
    bool archive_complete = false;
};

ServeResult serve_client(int client, RawStoreState &state,
                         FrameRetentionBuffer &retention,
                         neotape::ClosableQueue<RecordOrDone> &frame_queue,
                         const Options &opts, const string &archive_uuid,
                         const std::function<string()> &get_input_error_text) {
    bool volume_committed = false;
    uint64_t frames_served = 0;
    uint64_t next_send_seq = state.last_acked_global_frame == 0
                                 ? 1
                                 : state.last_acked_global_frame + 1;
    std::optional<uint64_t> archive_end_seq;

    auto outstanding_frames = [&]() -> uint64_t {
        return (next_send_seq - 1) - state.last_acked_global_frame;
    };

    auto process_ack_payload = [&](const std::vector<std::byte> &payload) {
        if (payload.size() != 8) {
            send_error(client, "ack_frame payload must be 8 bytes");
            return -1;
        }
        uint64_t const g = le64_from_bytes(payload);
        uint64_t const expected = state.last_acked_global_frame + 1;
        if (g != expected) {
            send_error(client,
                       format("ack {} out of order; expected {}", g, expected)
                           .c_str());
            return -1;
        }

        if (!volume_committed) {
            volume_committed = true;
            NEOTAPE_DEBUG("raw-store: volume {} committed\n",
                          state.next_volume_seq_num);
        }

        state.last_acked_global_frame = g;
        retention.ack(g);
        if (archive_end_seq.has_value() && g >= *archive_end_seq) {
            NEOTAPE_DEBUG("raw-store: archive end acked\n");
            return 1;
        }
        return 0;
    };

    auto wait_until_outstanding_below = [&](uint64_t limit) {
        while (outstanding_frames() >= limit) {
            auto ack = neotape::tcp::read_message(client);
            if (!ack.has_value()) {
                return -1;
            }
            if (ack->type != MessageType::ack_frame) {
                send_error(client, "send window full; expected ack_frame");
                return -1;
            }
            int const ack_result = process_ack_payload(ack->payload);
            if (ack_result != 0) {
                return ack_result;
            }
        }
        return 0;
    };

    try {
        for (;;) {
            auto req = neotape::tcp::read_message(client);
            if (!req.has_value()) {
                break;
            }

            if (archive_end_seq.has_value() &&
                req->type != MessageType::ack_frame) {
                send_error(client, "unexpected request after archive end");
                return ServeResult{true, volume_committed, frames_served};
            }

            switch (req->type) {
            case MessageType::next_frame: {
                int window_result =
                    wait_until_outstanding_below(opts.retention_frame_count);
                if (window_result < 0) {
                    return ServeResult{false, volume_committed, frames_served};
                }
                if (window_result > 0) {
                    return ServeResult{true, volume_committed, frames_served};
                }

                const std::vector<std::byte> *record_ptr =
                    retention.get(next_send_seq);
                uint64_t seq = 0;
                std::vector<std::byte> record;
                if (record_ptr != nullptr) {
                    record = *record_ptr;
                    seq = next_send_seq;
                    neotape::FrameHeader const hdr =
                        neotape::parse_fixed_header(
                            reinterpret_cast<const uint8_t *>(record.data()),
                            record.size());
                    if (hdr.channel_type == neotape::ChannelType::ARCHIVE_END) {
                        archive_end_seq = seq;
                    }
                } else {
                    auto next = frame_queue.pop();
                    if (!next.has_value()) {
                        string reason = get_input_error_text();
                        if (reason.empty()) {
                            reason = "frame queue closed";
                        }
                        send_error(client, reason.c_str());
                        return ServeResult{false, volume_committed,
                                           frames_served};
                    }
                    if (next->tape_eof) {
                        NEOTAPE_DEBUG("raw-store: sending tape_eof\n");
                        neotape::tcp::write_message(
                            client, Message{MessageType::tape_eof, {}});
                        break;
                    }
                    if (next->done) {
                        window_result = wait_until_outstanding_below(1);
                        if (window_result < 0) {
                            return ServeResult{false, volume_committed,
                                               frames_served};
                        }
                        if (window_result > 0) {
                            return ServeResult{true, volume_committed,
                                               frames_served};
                        }

                        uint64_t const ae_seq = next->global_seq_num + 1;
                        auto ae_record = build_archive_end_record(
                            opts.volume_block_size, state.next_volume_seq_num,
                            archive_uuid, opts.archive_name, ae_seq);
                        retention.add(ae_seq, ae_record);
                        archive_end_seq = ae_seq;
                        neotape::tcp::write_message(
                            client, Message{MessageType::frame_record,
                                            std::move(ae_record)});
                        ++frames_served;
                        ++next_send_seq;
                        break;
                    }
                    seq = next->global_seq_num;
                    if (seq != next_send_seq) {
                        send_error(client,
                                   "requested frame is no longer retained");
                        return ServeResult{false, volume_committed,
                                           frames_served};
                    }
                    record = std::move(next->record);
                    retention.add(seq, record);
                }

                if (record.size() != opts.volume_block_size) {
                    send_error(client, "frame size mismatch");
                    return ServeResult{false, volume_committed, frames_served};
                }
                patch_volume_seq_num(record, state.next_volume_seq_num);
                NEOTAPE_DEBUG("raw-store: sending frame global_seq={}\n", seq);
                neotape::tcp::write_message(
                    client,
                    Message{MessageType::frame_record, std::move(record)});
                ++frames_served;
                ++next_send_seq;
                break;
            }

            case MessageType::ack_frame: {
                int const ack_result = process_ack_payload(req->payload);
                if (ack_result < 0) {
                    return ServeResult{false, volume_committed, frames_served};
                }
                if (ack_result > 0) {
                    return ServeResult{true, volume_committed, frames_served};
                }
                break;
            }

            case MessageType::tape_eof:
                send_error(client, "unexpected tape_eof request");
                return ServeResult{false, volume_committed, frames_served};

            default:
                send_error(client, "unexpected request type");
                return ServeResult{false, volume_committed, frames_served};
            }
        }
    } catch (const std::exception &e) {
        NEOTAPE_DEBUG("raw-store: client error: {}\n", e.what());
        return ServeResult{false, volume_committed, frames_served};
    } catch (...) {
        NEOTAPE_DEBUG("raw-store: unknown client error\n");
        return ServeResult{false, volume_committed, frames_served};
    }

    return ServeResult{false, volume_committed, frames_served};
}

uint64_t run_raw_store(FILE *input, const Options &opts) {
    int const listener = create_listener(opts.listen_address);
    FdGuard const listener_guard(listener);
    std::cerr << format("raw-store listening on {}\n", opts.listen_address);

    string const archive_uuid = neotape::make_uuid_v4();
    RawFrameBuilder builder(opts.volume_block_size, archive_uuid,
                            opts.archive_name);
    neotape::ClosableQueue<RecordOrDone> frame_queue(8);

    std::exception_ptr input_error;
    string input_error_text;
    std::mutex input_error_mtx;

    auto capture_input_error = [&](const string &text) {
        std::scoped_lock const lock(input_error_mtx);
        if (!input_error) {
            input_error_text = text;
            input_error = std::current_exception();
        }
    };

    auto get_input_error_text = [&]() -> string {
        std::scoped_lock const lock(input_error_mtx);
        return input_error_text;
    };

    auto check_input_error = [&]() {
        std::scoped_lock const lock(input_error_mtx);
        if (input_error) {
            std::rethrow_exception(input_error);
        }
    };

    std::thread input_thread([&]() {
        try {
            produce_raw_frames(input, builder, frame_queue);
        } catch (const std::exception &e) {
            capture_input_error(e.what());
            frame_queue.close();
        } catch (...) {
            capture_input_error("unknown input error");
            frame_queue.close();
        }
    });
    ThreadJoiner input_joiner(input_thread);

    FrameRetentionBuffer retention(opts.retention_frame_count);
    RawStoreState state;
    uint64_t total_frames_served = 0;

    try {
        while (!state.archive_complete) {
            int const client = accept(listener, nullptr, nullptr);
            if (client < 0) {
                int const saved_errno = errno;
                throw std::runtime_error(
                    format("accept: {}", std::strerror(saved_errno)));
            }
            FdGuard const client_guard(client);
            NEOTAPE_DEBUG("raw-store: accepted connection for volume seq={}\n",
                          state.next_volume_seq_num);

            ServeResult result =
                serve_client(client, state, retention, frame_queue, opts,
                             archive_uuid, get_input_error_text);
            total_frames_served += result.frames_served;

            if (result.archive_complete) {
                std::cerr << format(
                    "raw-store: archive complete, served {} frames on this "
                    "connection\n",
                    result.frames_served);
                state.archive_complete = true;
            } else if (result.volume_committed) {
                std::cerr << format(
                    "raw-store: connection closed, volume {} committed, "
                    "advancing to seq={}\n",
                    state.next_volume_seq_num, state.next_volume_seq_num + 1);
                ++state.next_volume_seq_num;
            } else {
                std::cerr << format(
                    "raw-store: connection closed before commit, reusing "
                    "volume seq={}\n",
                    state.next_volume_seq_num);
            }
        }
    } catch (...) {
        frame_queue.close();
        if (input_thread.joinable()) {
            input_thread.join();
        }
        check_input_error();
        throw;
    }

    frame_queue.close();
    if (input_thread.joinable()) {
        input_thread.join();
    }
    check_input_error();
    return total_frames_served;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.debug;

        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            fail("failed to ignore SIGPIPE");
        }

        FILE *raw_input = nullptr;
        bool owned = false;
        if (opts.input_name == "-") {
            raw_input = stdin;
        } else {
            raw_input = std::fopen(opts.input_name.c_str(), "rb");
            if (raw_input == nullptr) {
                fail_errno(string("open ") + opts.input_name);
            }
            owned = true;
        }
        FileGuard const input_guard(raw_input, owned);

        uint64_t const served = run_raw_store(raw_input, opts);
        std::cerr << format("raw-store served {} frames\n", served);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
