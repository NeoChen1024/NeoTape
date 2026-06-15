#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_ioctl.h"
#include "neotape/tcp_protocol.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using neotape::tcp::Address;
using neotape::tcp::parse_address;
using std::format;
using std::string;
using std::vector;

struct FdGuard {
    int fd = -1;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() {
        if (fd >= 0)
            ::close(fd);
    }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
};

struct TargetLocator {
    enum Kind { none, tape, spool } kind = none;
    std::string path;
};

TargetLocator parse_target(const std::string &s) {
    if (s.rfind("tape:", 0) == 0)
        return {TargetLocator::tape, s.substr(5)};
    if (s.rfind("spool:", 0) == 0)
        return {TargetLocator::spool, s.substr(6)};
    throw std::runtime_error(
        "target must be tape:<device> or spool:<dir>");
}

struct Options {
    string source_address;
    TargetLocator target;
    bool erase = false;
    bool append = false;
    size_t output_buffer_size = 256ull * 1024 * 1024;
    std::optional<uint64_t> max_volume_bytes;
    bool debug = false;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(2);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --source <tcp://host:port|unix://path>\n"
        "       --target <tape:/dev/nst0|spool:./dir>\n"
        "       [--erase | --append]\n"
        "       [--output-buffer-size <bytes>]\n"
        "       [--max-volume-bytes <bytes>] [--debug]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"target", required_argument, nullptr, 256},
        {"erase", no_argument, nullptr, 257},
        {"append", no_argument, nullptr, 258},
        {"output-buffer-size", required_argument, nullptr, 259},
        {"max-volume-bytes", required_argument, nullptr, 260},
        {"debug", no_argument, nullptr, 261},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "s:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source_address = optarg;
            break;
        case 256:
            opts.target = parse_target(optarg);
            break;
        case 257:
            opts.erase = true;
            break;
        case 258:
            opts.append = true;
            break;
        case 259: {
            try {
                opts.output_buffer_size = static_cast<size_t>(
                    neotape::parse_size(optarg, "output buffer size"));
            } catch (const std::exception &e) {
                std::cerr << format("neotape-write: {}\n", e.what());
                std::exit(2);
            }
            break;
        }
        case 260: {
            try {
                opts.max_volume_bytes = neotape::parse_size(
                    optarg, "max volume bytes");
            } catch (const std::exception &e) {
                std::cerr << format("neotape-write: {}\n", e.what());
                std::exit(2);
            }
            break;
        }
        case 261:
            opts.debug = true;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.source_address.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (opts.target.kind == TargetLocator::none)
        usage_error("--target is required");
    if (opts.erase && opts.append)
        usage_error("--erase and --append are mutually exclusive");

    if (opts.max_volume_bytes.has_value() &&
        opts.target.kind != TargetLocator::spool)
        usage_error("--max-volume-bytes is only valid with spool targets");

    constexpr size_t min_output_buffer_size = 8ull * 1024 * 1024;
    if (opts.output_buffer_size < min_output_buffer_size)
        usage_error("--output-buffer-size must be at least 8 MiB");

    return opts;
}

int connect_to_source(const string &addr) {
    Address a = parse_address(addr);

    int fd = -1;
    if (a.is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            fail(format("socket: {}", std::strerror(errno)));
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (a.path.size() >= sizeof(sa.sun_path))
            fail("unix socket path too long");
        std::memcpy(sa.sun_path, a.path.data(), a.path.size());
        if (connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0)
            fail(format("connect {}: {}", a.path, std::strerror(errno)));
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        int gai = getaddrinfo(a.host.c_str(), a.port.c_str(), &hints, &res);
        if (gai != 0)
            fail(format("getaddrinfo: {}", gai_strerror(gai)));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res_guard(res,
                                                                     freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0)
            fail(format("socket: {}", std::strerror(errno)));
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
            fail(format("connect {}:{}: {}", a.host, a.port, std::strerror(errno)));
    }
    return fd;
}

struct PendingFrame {
    uint64_t global_seq_num;
    vector<std::byte> record;
    bool is_filemark = false;
};

struct WriterState {
    std::deque<PendingFrame> output_queue;
    std::mutex output_mtx;
    std::condition_variable output_cv;
    std::atomic<bool> writer_stop{false};
    std::atomic<bool> writer_error{false};
    string writer_error_text;
    std::atomic<uint64_t> last_written_seq{0};
    std::atomic<bool> eot_reached{false};
    std::atomic<bool> final_drain{false};
};

// Do not write a trailing filemark when EOT has been reached. On real tape
// hardware, issuing MTWEOF near the physical end of the medium can block while
// the kernel tries to flush data that cannot fit. The next volume resumes with
// the next uncommitted frame.
void write_trailing_filemark(mt::TapeDevice *dev) {
    (void)dev;
    NEOTAPE_DEBUG("writer: omitting trailing filemark at EOT/EOD\n");
}

std::vector<std::byte> uint64_to_le_bytes(uint64_t v) {
    std::vector<std::byte> out(8);
    for (size_t i = 0; i < 8; ++i)
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
    return out;
}

class CapacityLimitedTapeDevice : public mt::TapeDevice {
  public:
    CapacityLimitedTapeDevice(std::unique_ptr<mt::TapeDevice> inner,
                              uint64_t max_bytes)
        : mt::TapeDevice(-1, inner->device_path(), inner->is_read_write()),
          inner_(std::move(inner)), max_bytes_(max_bytes) {}

    int fd() const noexcept override { return inner_->fd(); }

    void write_record(const void *data, std::size_t size) override {
        if (written_ + size > max_bytes_)
            throw mt::Error(device_path(), "capacity limit", ENOSPC);
        inner_->write_record(data, size);
        written_ += size;
    }

  protected:
    void do_mtop(int op, int count) override {
        switch (op) {
        case mt::MTWEOF: inner_->write_filemark(count); return;
        case mt::MTREW: inner_->rewind(); return;
        case mt::MTEOM: inner_->space_to_eod(); return;
        case mt::MTFSF: inner_->space_fwd(count); return;
        case mt::MTBSF: inner_->space_bwd(count); return;
        case mt::MTFSFM: inner_->space_fwd_filemark(count); return;
        case mt::MTBSFM: inner_->space_bwd_filemark(count); return;
        case mt::MTFSR: inner_->space_fwd_records(count); return;
        case mt::MTBSR: inner_->space_bwd_records(count); return;
        case mt::MTFSS: inner_->space_fwd_setmarks(count); return;
        case mt::MTBSS: inner_->space_bwd_setmarks(count); return;
        case mt::MTSEEK: inner_->seek_block(count); return;
        case mt::MTSETBLK: inner_->set_block_size(count); return;
        case mt::MTSETDENSITY: inner_->set_density(count); return;
        case mt::MTCOMPRESSION: inner_->set_compression(count != 0); return;
        case mt::MTLOCK: inner_->lock(); return;
        case mt::MTUNLOCK: inner_->unlock(); return;
        case mt::MTLOAD: inner_->load(count); return;
        case mt::MTOFFL: inner_->offline(); return;
        case mt::MTERASE: inner_->erase(count); return;
        default: throw mt::Error(device_path(), "mtop", ENOTSUP);
        }
    }

    mt::Position do_tell() override { return inner_->tell(); }
    mt::Status do_status() override { return inner_->status(); }

  private:
    std::unique_ptr<mt::TapeDevice> inner_;
    uint64_t max_bytes_;
    uint64_t written_ = 0;
};

void tape_writer_thread(mt::TapeDevice *dev, int fd,
                        std::mutex &socket_write_mtx, WriterState &state) {
    using neotape::tcp::Message;
    using neotape::tcp::MessageType;

    for (;;) {
        std::unique_lock lock(state.output_mtx);
        state.output_cv.wait(lock, [&] {
            return !state.output_queue.empty() || state.writer_stop.load();
        });
        if (state.output_queue.empty() && state.writer_stop.load()) {
            NEOTAPE_DEBUG("writer_thread: stopping (queue empty + stop)\n");
            return;
        }
        auto frame = std::move(state.output_queue.front());
        state.output_queue.pop_front();
        lock.unlock();

        if (frame.is_filemark) {
            NEOTAPE_DEBUG("writer_thread: writing filemark\n");
            try {
                dev->write_filemark();
            } catch (const std::exception &e) {
                state.writer_error_text = e.what();
                state.writer_error.store(true);
                return;
            }
            continue;
        }

        bool status_eot = false;
        NEOTAPE_DEBUG(
            "writer_thread: frame global_seq={} record_size={}\n",
            frame.global_seq_num, frame.record.size());
        try {
            status_eot = dev->status().eot();
        } catch (const mt::Error &e) {
            if (e.error_code() == ENOSPC) {
                NEOTAPE_DEBUG(
                    "writer_thread: pre-write status ENOSPC, treating as EOT\n");
                state.eot_reached.store(true);
                return;
            }
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        }
        if (status_eot) {
            NEOTAPE_DEBUG(
                "writer_thread: pre-write status EOT, treating as EOT\n");
            state.eot_reached.store(true);
            return;
        }

        try {
            dev->write_record(frame.record.data(), frame.record.size());
            state.last_written_seq.store(frame.global_seq_num);
        } catch (const mt::Error &e) {
            if (e.error_code() == ENOSPC) {
                NEOTAPE_DEBUG(
                    "writer_thread: write_record ENOSPC, treating as EOT\n");
                state.eot_reached.store(true);
                return;
            }
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        } catch (const std::exception &e) {
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        }

        NEOTAPE_DEBUG(
            "writer_thread: frame global_seq={} written\n",
            frame.global_seq_num);

        status_eot = false;
        try {
            status_eot = dev->status().eot();
        } catch (const mt::Error &e) {
            if (e.error_code() == ENOSPC) {
                NEOTAPE_DEBUG(
                    "writer_thread: post-write status ENOSPC, treating as EOT\n");
                state.eot_reached.store(true);
                return;
            }
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        }
        if (status_eot) {
            NEOTAPE_DEBUG(
                "writer_thread: post-write status EOT, treating as EOT\n");
            state.eot_reached.store(true);
            return;
        }

        try {
            std::lock_guard write_lock(socket_write_mtx);
            if (state.final_drain.load()) {
                NEOTAPE_DEBUG(
                    "writer_thread: final drain, skipping ack global_seq={}\n",
                    frame.global_seq_num);
            } else {
                NEOTAPE_DEBUG(
                    "writer_thread: sending ack global_seq={}\n",
                    frame.global_seq_num);
                neotape::tcp::write_message(
                    fd, Message{MessageType::ack_frame,
                                uint64_to_le_bytes(frame.global_seq_num)});
                NEOTAPE_DEBUG(
                    "writer_thread: ack sent global_seq={}\n",
                    frame.global_seq_num);
            }
        } catch (const std::exception &e) {
            // If the archiver has already closed its end (archive complete),
            // the ACK is not needed; treat this as a clean shutdown.
            const char *what = e.what();
            if (std::strstr(what, "EPIPE") != nullptr || std::strstr(what, "Broken pipe") != nullptr) {
                NEOTAPE_DEBUG(
                    "writer_thread: ack got EPIPE, clean shutdown\n");
                return;
            }
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        }
    }
}

struct WriterThreadJoiner {
    WriterState *state = nullptr;
    std::thread *thread = nullptr;
    bool joined = false;
    WriterThreadJoiner(WriterState &s, std::thread &t)
        : state(&s), thread(&t) {}
    void join() {
        if (!joined && state != nullptr && thread != nullptr) {
            state->writer_stop.store(true);
            state->output_cv.notify_all();
            if (thread->joinable())
                thread->join();
            joined = true;
        }
    }
    ~WriterThreadJoiner() { join(); }
    WriterThreadJoiner(const WriterThreadJoiner &) = delete;
    WriterThreadJoiner &operator=(const WriterThreadJoiner &) = delete;
};

} // namespace

int main(int argc, char **argv) {
    // Acks are sent from the writer thread while the main thread reads
    // responses.  If the archiver closes the connection because the archive
    // is complete, an in-flight ACK could raise SIGPIPE; ignore it and handle
    // the resulting EPIPE as a clean shutdown.
    std::signal(SIGPIPE, SIG_IGN);

    try {
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.debug;

        FdGuard fd_guard(connect_to_source(opts.source_address));
        int fd = fd_guard.fd;

        using neotape::tcp::Message;
        using neotape::tcp::MessageType;

        std::optional<uint32_t> volume_block_size;

        // Output abstraction: a tape or spool device.  There is no raw file
        // mode; spool is the filesystem surrogate for tape.
        struct TargetOutput {
            std::unique_ptr<mt::TapeDevice> device;
        };
        TargetOutput output;

        {
            std::unique_ptr<mt::TapeDevice> dev;
            if (opts.target.kind == TargetLocator::tape) {
                dev = std::make_unique<mt::TapeDevice>(opts.target.path, true);
            } else {
                auto spool = std::make_unique<mt::SpoolTapeDevice>(
                    std::filesystem::path(opts.target.path), true);
                if (opts.max_volume_bytes.has_value()) {
                    dev = std::make_unique<CapacityLimitedTapeDevice>(
                        std::move(spool), *opts.max_volume_bytes);
                } else {
                    dev = std::move(spool);
                }
            }

            // Default policy: refuse to overwrite existing tape content.
            if (!opts.erase && !opts.append) {
                auto st = dev->status();
                if (!st.bot())
                    fail("tape is not at BOT; use --erase or --append");
                // BOT alone doesn't guarantee the tape is empty.  If the
                // drive reports EOD at BOT, the tape is empty; otherwise
                // there is at least one record after BOT.
                if (!st.eod())
                    fail("tape appears to contain data; use --erase or --append");
            }

            if (opts.append)
                dev->space_to_eod();
            else
                dev->rewind(); // --erase or empty tape

            output = TargetOutput{std::move(dev)};
        }

        auto write_bytes = [&](const std::vector<std::byte> &bytes) {
            output.device->write_record(bytes.data(), bytes.size());
        };

        WriterState wstate;
        std::mutex socket_write_mtx;

        std::thread writer_thread(tape_writer_thread,
                                  output.device.get(),
                                  fd,
                                  std::ref(socket_write_mtx),
                                  std::ref(wstate));
        WriterThreadJoiner joiner(wstate, writer_thread);

        auto joined_fail = [&](const string &msg) {
            joiner.join();
            fail(msg);
        };

        auto write_msg = [&](const Message &msg) {
            std::lock_guard lock(socket_write_mtx);
            neotape::tcp::write_message(fd, msg);
        };

        for (;;) {
            if (wstate.writer_error.load())
                joined_fail(wstate.writer_error_text);

            if (wstate.eot_reached.load()) {
                NEOTAPE_DEBUG("writer: eot_reached, joining thread\n");
                joiner.join();
                write_trailing_filemark(output.device.get());
                uint64_t final_seq = wstate.last_written_seq.load();
                if (final_seq > 0) {
                    write_msg(Message{MessageType::ack_frame,
                                      uint64_to_le_bytes(final_seq)});
                }
                std::cerr << format(
                    "writer: reached end of tape after {} frames\n", final_seq);
                return 1;
            }

            // Enforce output buffer limit.
            {
                std::unique_lock lock(wstate.output_mtx);
                size_t queued_bytes = 0;
                for (const auto &f : wstate.output_queue)
                    queued_bytes += f.record.size();
                if (queued_bytes >= opts.output_buffer_size) {
                    lock.unlock();
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                    continue;
                }
            }

            NEOTAPE_DEBUG("writer: requesting next frame\n");
            write_msg(Message{MessageType::next_frame});
            auto msg = neotape::tcp::read_message(fd);
            if (!msg)
                joined_fail("unexpected disconnect");
            NEOTAPE_DEBUG("writer: received msg type={}\n",
                          static_cast<int>(msg->type));

            switch (msg->type) {
            case MessageType::frame_record: {
                neotape::FrameHeader header = neotape::parse_fixed_header(
                    reinterpret_cast<const uint8_t *>(msg->payload.data()),
                    msg->payload.size());
                uint32_t record_size = neotape::decoded_block_size(header);
                if (!volume_block_size.has_value()) {
                    volume_block_size = record_size;
                    std::cerr << format("writer: first frame parsed block_size={}\n",
                                        record_size);
                }
                if (msg->payload.size() != *volume_block_size)
                    joined_fail(format("frame size mismatch: expected {}, got {}",
                                       *volume_block_size, msg->payload.size()));

                if (header.channel_type == neotape::ChannelType::ARCHIVE_END) {
                    NEOTAPE_DEBUG("writer: archive_end frame, draining queue\n");
                    {
                        std::lock_guard lock(wstate.output_mtx);
                        wstate.final_drain.store(true);
                    }
                    wstate.output_cv.notify_all();
                    for (;;) {
                        std::unique_lock lock(wstate.output_mtx);
                        if (wstate.output_queue.empty())
                            break;
                        lock.unlock();
                        if (wstate.writer_error.load())
                            joined_fail(wstate.writer_error_text);
                        if (wstate.eot_reached.load()) {
                            joiner.join();
                            write_trailing_filemark(output.device.get());
                            uint64_t final_seq = wstate.last_written_seq.load();
                            if (final_seq > 0)
                                write_msg(Message{MessageType::ack_frame,
                                                  uint64_to_le_bytes(final_seq)});
                            std::cerr << format("writer: reached end of tape after {} frames\n",
                                                final_seq);
                            return 1;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    joiner.join();
                    write_bytes(msg->payload);
                    write_msg(Message{MessageType::ack_frame,
                                      uint64_to_le_bytes(header.global_frame_seq_num)});
                    std::cerr << format("writer: received archive end at frame {}\n",
                                        header.global_frame_seq_num);
                    return 0;
                }

                uint64_t gseq = header.global_frame_seq_num;
                std::unique_lock lock(wstate.output_mtx);
                wstate.output_queue.push_back(PendingFrame{gseq, std::move(msg->payload)});
                wstate.output_cv.notify_one();
                break;
            }
            case MessageType::tape_eof: {
                NEOTAPE_DEBUG("writer: pushing filemark to queue\n");
                std::unique_lock lock(wstate.output_mtx);
                wstate.output_queue.push_back(PendingFrame{0, {}, true});
                wstate.output_cv.notify_one();
                break;
            }

            case MessageType::error: {
                joiner.join();
                string reason;
                reason.reserve(msg->payload.size());
                for (std::byte b : msg->payload)
                    reason.push_back(static_cast<char>(b));
                if (reason.empty())
                    reason = "archiver reported error";
                std::cerr << format("neotape-write: {}\n", reason);
                std::exit(2);
            }
            default:
                joined_fail(format("unexpected message type {}",
                            static_cast<int>(msg->type)));
            }
        }
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
