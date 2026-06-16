#include "neotape/closable_queue.hpp"
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/tcp_server.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <functional>
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

namespace neotape {

namespace {

using neotape::tcp::Address;
using neotape::tcp::Message;
using neotape::tcp::MessageType;
using neotape::tcp::parse_address;

int create_listener(const std::string &addr) {
    Address a = parse_address(addr);

    int fd = -1;
    if (a.is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error(
                std::format("socket: {}", std::strerror(errno)));
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
                std::format("bind {}: {}", a.path, std::strerror(errno)));
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
                std::format("getaddrinfo: {}", gai_strerror(gai)));
        }
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> const res_guard(
            res, freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            throw std::runtime_error(
                std::format("socket: {}", std::strerror(errno)));
        }
        int yes = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
            throw std::runtime_error(std::format("bind {}:{}: {}", a.host,
                                                 a.port, std::strerror(errno)));
        }
    }

    if (listen(fd, 1) < 0) {
        throw std::runtime_error(
            std::format("listen: {}", std::strerror(errno)));
    }
    return fd;
}

void copy_header_to_record(const HeaderBytes &header,
                           std::vector<std::byte> &record) {
    for (std::size_t i = 0; i < header.size(); ++i) {
        record[i] = static_cast<std::byte>(header[i]);
    }
}

void finalize_record_hash(FrameHeader &header, std::vector<std::byte> &record) {
    HeaderBytes header_bytes = serialize_frame_header(header);
    copy_header_to_record(header_bytes, record);
    header.frame_hash = compute_frame_hash(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
    header_bytes = serialize_frame_header(header);
    copy_header_to_record(header_bytes, record);
}

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
        while (!frames_.empty() &&
               frames_.front().global_seq_num <= global_seq_num) {
            frames_.pop_front();
        }
    }

    [[nodiscard]] bool has(uint64_t global_seq_num) const {
        for (const auto &f : frames_) {
            if (f.global_seq_num == global_seq_num) {
                return true;
            }
        }
        return false;
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

    [[nodiscard]] uint64_t lowest_available() const {
        if (frames_.empty()) {
            return 0;
        }
        return frames_.front().global_seq_num;
    }

    [[nodiscard]] bool empty() const { return frames_.empty(); }

  private:
    size_t max_frames_;
    std::deque<RetainedFrame> frames_;
};

struct FrameBuilder {
    uint32_t block_size;
    std::string archive_uuid;
    std::string archive_name;
    uint64_t global_frame = 1;
    uint64_t current_slice = 1;
    std::vector<std::byte> pending;

    explicit FrameBuilder(uint32_t bs, std::string uuid, std::string name)
        : block_size(bs), archive_uuid(std::move(uuid)),
          archive_name(std::move(name)) {}

    void set_current_slice(uint64_t s) { current_slice = s; }

    [[nodiscard]] uint32_t payload_capacity() const {
        return block_size - fixed_header_size;
    }

    // Append payload bytes. Returns zero or more complete frames.
    std::vector<std::vector<std::byte>> feed(std::span<const std::byte> bytes,
                                             std::vector<uint64_t> &seq_nums) {
        std::vector<std::vector<std::byte>> out;
        pending.insert(pending.end(), bytes.begin(), bytes.end());
        const uint32_t cap = payload_capacity();
        while (pending.size() >= cap) {
            uint64_t const seq = global_frame++;
            seq_nums.push_back(seq);
            out.push_back(build_frame(std::span(pending.begin(), cap), seq));
            pending.erase(pending.begin(), pending.begin() + cap);
        }
        return out;
    }

    // Force any remaining pending bytes into a final frame.
    std::optional<std::pair<std::vector<std::byte>, uint64_t>> flush() {
        if (pending.empty()) {
            return std::nullopt;
        }
        uint64_t const seq = global_frame++;
        auto rec = build_frame(std::span(pending), seq);
        pending.clear();
        return std::pair{std::move(rec), seq};
    }

    [[nodiscard]] std::vector<std::byte>
    build_frame(std::span<const std::byte> payload, uint64_t seq_num) const {
        assert(payload.size() <= payload_capacity());

        FrameHeader fh;
        fh.channel_type = ChannelType::CH_CONTENT;
        fh.volume_block_size_kib = static_cast<uint16_t>(block_size / 1024U);
        fh.archive_uuid = archive_uuid;
        fh.archive_label = archive_name;
        fh.volume_seq_num = 0;
        fh.global_frame_seq_num = seq_num;
        fh.logical_slice_seq_num = current_slice;
        fh.frame_seq_num_within_channel = 1;
        fh.frame_payload_size = payload.size();
        fh.flags = frame_flag_start | frame_flag_end;

        HeaderBytes const header = serialize_frame_header(fh);
        std::vector<std::byte> record(block_size, std::byte{0});
        copy_header_to_record(header, record);
        std::copy(payload.begin(), payload.end(),
                  record.begin() +
                      static_cast<std::ptrdiff_t>(fixed_header_size));
        finalize_record_hash(fh, record);
        return record;
    }
};

std::vector<std::byte> build_archive_end_record(uint32_t block_size,
                                                uint64_t volume_seq_num,
                                                const std::string &archive_uuid,
                                                const std::string &archive_name,
                                                uint64_t global_seq_num) {
    FrameHeader h;
    h.channel_type = ChannelType::ARCHIVE_END;
    h.volume_block_size_kib = static_cast<uint16_t>(block_size / 1024U);
    h.archive_uuid = archive_uuid;
    h.archive_label = archive_name;
    h.volume_seq_num = volume_seq_num;
    h.global_frame_seq_num = global_seq_num;
    h.logical_slice_seq_num = 0;
    h.frame_seq_num_within_channel = 1;
    h.frame_payload_size = 0;
    h.flags = frame_flag_start | frame_flag_end | frame_flag_clean_end;

    std::vector<std::byte> record(block_size, std::byte{0});
    finalize_record_hash(h, record);
    return record;
}

struct RecordOrDone {
    std::vector<std::byte> record;
    uint64_t global_seq_num = 0;
    uint64_t last_slice_seq_num = 0;
    bool tape_eof = false;
    bool done = false;
};

struct ThreadJoiner {
    std::thread &thread;
    explicit ThreadJoiner(std::thread &t) : thread(t) {}
    ~ThreadJoiner() {
        if (thread.joinable()) {
            thread.join();
        }
    }
    ThreadJoiner(const ThreadJoiner &) = delete;
    ThreadJoiner &operator=(const ThreadJoiner &) = delete;
    ThreadJoiner(ThreadJoiner &&) = delete;
    ThreadJoiner &operator=(ThreadJoiner &&) = delete;
};

struct FdGuard {
    int fd = -1;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() {
        if (fd >= 0) {
            close(fd);
        }
    }
    void release() { fd = -1; }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
    FdGuard(FdGuard &&) = delete;
    FdGuard &operator=(FdGuard &&) = delete;
};

void patch_volume_seq_num(std::vector<std::byte> &record,
                          uint64_t new_volume_seq_num) {
    FrameHeader hdr = parse_fixed_header(
        reinterpret_cast<const uint8_t *>(record.data()), record.size());
    if (hdr.volume_seq_num == new_volume_seq_num) {
        return;
    }
    hdr.volume_seq_num = new_volume_seq_num;
    finalize_record_hash(hdr, record);
}

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

PaxWriterCallbacks make_server_callbacks(FrameBuilder &builder,
                                         ClosableQueue<RecordOrDone> &queue) {
    return PaxWriterCallbacks{
        .begin_slice =
            [&](uint64_t slice_num) {
                builder.set_current_slice(slice_num + 1);
            },
        .write_chunk =
            [&](PaxChunk chunk) {
                std::vector<uint64_t> seq_nums;
                auto frames = builder.feed(chunk.bytes, seq_nums);
                assert(frames.size() == seq_nums.size());
                for (std::size_t i = 0; i < frames.size(); ++i) {
                    if (!queue.push(RecordOrDone{std::move(frames[i]),
                                                 seq_nums[i], 0, false,
                                                 false})) {
                        throw std::runtime_error("frame consumer disconnected");
                    }
                }
            },
        .end_slice =
            [&](uint64_t slice_num) {
                if (auto tail = builder.flush(); tail.has_value()) {
                    if (!queue.push(RecordOrDone{std::move(tail->first),
                                                 tail->second, 0, false,
                                                 false})) {
                        throw std::runtime_error("frame consumer disconnected");
                    }
                }
                if (!queue.push(RecordOrDone{{}, 0, slice_num, true, false})) {
                    throw std::runtime_error("frame consumer disconnected");
                }
            },
        .progress_paused = [] { return false; },
    };
}

struct ServeResult {
    bool archive_complete = false;
    bool volume_committed = false;
    uint64_t frames_served = 0;
};

struct TcpArchiverState {
    uint64_t next_volume_seq_num;
    uint64_t last_acked_global_frame;
    bool archive_complete;
};

ServeResult
serve_client(int client, TcpArchiverState &state,
             FrameRetentionBuffer &retention,
             ClosableQueue<RecordOrDone> &frame_queue,
             const TcpArchiverOptions &opts, const std::string &archive_uuid,
             const std::function<std::string()> &get_pax_error_text) {
    bool volume_committed = false;
    uint64_t frames_served = 0;
    uint64_t next_send_seq = state.last_acked_global_frame == 0
                                 ? 1
                                 : state.last_acked_global_frame + 1;
    std::optional<uint64_t> archive_end_seq;

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
                if (!volume_committed) {
                    volume_committed = true;
                    NEOTAPE_DEBUG("archiver: volume {} committed\n",
                                  state.next_volume_seq_num);
                }

                const std::vector<std::byte> *record_ptr =
                    retention.get(next_send_seq);
                uint64_t seq = 0;
                std::vector<std::byte> record;
                if (record_ptr != nullptr) {
                    record = *record_ptr;
                    seq = next_send_seq;
                    FrameHeader const hdr = parse_fixed_header(
                        reinterpret_cast<const uint8_t *>(record.data()),
                        record.size());
                    if (hdr.channel_type == ChannelType::ARCHIVE_END) {
                        archive_end_seq = seq;
                    }
                } else {
                    auto next = frame_queue.pop();
                    if (!next.has_value()) {
                        std::string reason = get_pax_error_text();
                        if (reason.empty()) {
                            reason = "frame queue closed";
                        }
                        send_error(client, reason.c_str());
                        return ServeResult{false, volume_committed,
                                           frames_served};
                    }
                    if (next->tape_eof) {
                        NEOTAPE_DEBUG("archiver: sending tape_eof\n");
                        neotape::tcp::write_message(
                            client, Message{MessageType::tape_eof, {}});
                        break;
                    }
                    if (next->done) {
                        uint64_t ae_seq = next->global_seq_num + 1;
                        auto ae_record = build_archive_end_record(
                            opts.volume_block_size, state.next_volume_seq_num,
                            archive_uuid, opts.archive_name, ae_seq);
                        retention.add(ae_seq, ae_record);
                        seq = ae_seq;
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
                NEOTAPE_DEBUG("archiver: sending frame global_seq={}\n", seq);
                neotape::tcp::write_message(
                    client,
                    Message{MessageType::frame_record, std::move(record)});
                ++frames_served;
                ++next_send_seq;
                break;
            }

            case MessageType::ack_frame: {
                if (req->payload.size() != 8) {
                    send_error(client, "ack_frame payload must be 8 bytes");
                    return ServeResult{false, volume_committed, frames_served};
                }
                uint64_t g = le64_from_bytes(req->payload);

                // Reject ACKs for frames we haven't sent.
                if (g >= next_send_seq) {
                    send_error(client,
                               std::format("ack {} ahead of sent range "
                                           "[1, {})",
                                           g, next_send_seq)
                                   .c_str());
                    return ServeResult{false, volume_committed, frames_served};
                }

                // Reject ACKs behind what we've already committed.
                if (g <= state.last_acked_global_frame) {
                    NEOTAPE_DEBUG("archiver: stale ack global_seq={}\n", g);
                    break;
                }

                NEOTAPE_DEBUG("archiver: ack frame global_seq={}\n", g);
                state.last_acked_global_frame = g;
                retention.ack(state.last_acked_global_frame);
                if (archive_end_seq.has_value() && g >= *archive_end_seq) {
                    NEOTAPE_DEBUG("archiver: archive end acked\n");
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
        NEOTAPE_DEBUG("archiver: client error: {}\n", e.what());
        return ServeResult{false, volume_committed, frames_served};
    } catch (...) {
        NEOTAPE_DEBUG("archiver: unknown client error\n");
        return ServeResult{false, volume_committed, frames_served};
    }

    return ServeResult{false, volume_committed, frames_served};
}

} // namespace

uint64_t run_tcp_archiver(const TcpArchiverOptions &opts) {
    if (!valid_block_size(opts.volume_block_size)) {
        throw std::runtime_error("invalid volume block size");
    }

    if (!opts.use_pax) {
        int const listener = create_listener(opts.listen_address);
        std::cerr << std::format("archiver listening on {}\n",
                                 opts.listen_address);

        int const client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            int const saved_errno = errno;
            close(listener);
            throw std::runtime_error(
                std::format("accept: {}", std::strerror(saved_errno)));
        }
        close(listener);

        std::string const archive_uuid = make_uuid_v4();
        FrameBuilder builder(opts.volume_block_size, archive_uuid,
                             opts.archive_name);

        constexpr uint64_t dummy_frame_count = 8;
        auto has_more_frames = [](uint64_t idx) {
            return idx < dummy_frame_count;
        };
        auto produce_record = [&](uint64_t idx) {
            const uint32_t cap = builder.payload_capacity();
            std::vector<std::byte> payload(cap);
            for (uint32_t i = 0; i < cap; ++i) {
                payload[i] =
                    static_cast<std::byte>(static_cast<uint8_t>(idx + i));
            }
            return builder.build_frame(std::span(payload), idx + 1);
        };
        uint64_t request_count = 0;
        uint64_t frames_served = 0;

        for (;;) {
            auto req = neotape::tcp::read_message(client);
            if (!req.has_value()) {
                break;
            }

            switch (req->type) {
            case MessageType::next_frame:
                if (!has_more_frames(frames_served)) {
                    auto ae_record = build_archive_end_record(
                        opts.volume_block_size, opts.initial_volume_seq_num,
                        archive_uuid, opts.archive_name, frames_served + 1);
                    neotape::tcp::write_message(
                        client, Message{MessageType::frame_record,
                                        std::move(ae_record)});
                    close(client);
                    return frames_served;
                }
                if (request_count % 4 == 3) {
                    neotape::tcp::write_message(
                        client, Message{MessageType::tape_eof, {}});
                } else {
                    auto rec = produce_record(frames_served);
                    if (rec.size() != opts.volume_block_size) {
                        throw std::runtime_error(
                            "produce_record size mismatch");
                    }
                    neotape::tcp::write_message(
                        client,
                        Message{MessageType::frame_record, std::move(rec)});
                    ++frames_served;
                }
                ++request_count;
                break;
            default:
                send_error(client, "unexpected request type");
                close(client);
                return frames_served;
            }
        }
        close(client);
        return frames_served;
    }

    int const listener = create_listener(opts.listen_address);
    FdGuard const listener_guard(listener);
    std::cerr << std::format("archiver listening on {}\n", opts.listen_address);

    std::string const archive_uuid = make_uuid_v4();
    FrameBuilder builder(opts.volume_block_size, archive_uuid,
                         opts.archive_name);
    ClosableQueue<RecordOrDone> frame_queue(8);

    std::exception_ptr pax_error;
    std::string pax_error_text;
    std::mutex pax_error_mtx;
    std::atomic<bool> cancelled{false};

    auto capture_pax_error = [&](const std::string &text) {
        std::scoped_lock const lock(pax_error_mtx);
        if (!pax_error) {
            pax_error_text = text;
            pax_error = std::current_exception();
        }
    };

    auto get_pax_error_text = [&]() -> std::string {
        std::scoped_lock const lock(pax_error_mtx);
        return pax_error_text;
    };

    auto check_pax_error = [&]() {
        std::scoped_lock const lock(pax_error_mtx);
        if (pax_error) {
            std::rethrow_exception(pax_error);
        }
    };

    std::thread pax_thread([&]() {
        try {
            auto callbacks = make_server_callbacks(builder, frame_queue);
            write_pax(opts.pax, std::move(callbacks));
            // Flush any trailing partial frame after last end_slice.
            if (auto tail = builder.flush(); tail.has_value()) {
                if (!frame_queue.push(RecordOrDone{std::move(tail->first),
                                                   tail->second, 0, false,
                                                   false})) {
                    throw std::runtime_error("frame consumer disconnected");
                }
            }
            uint64_t const last_global_seq =
                builder.global_frame == 1 ? 0 : builder.global_frame - 1;
            uint64_t const last_slice_seq = builder.current_slice;
            if (!frame_queue.push(RecordOrDone{
                    {}, last_global_seq, last_slice_seq, false, true})) {
                throw std::runtime_error("frame consumer disconnected");
            }
        } catch (const std::exception &e) {
            capture_pax_error(e.what());
            frame_queue.close();
        } catch (...) {
            capture_pax_error("unknown pax error");
            frame_queue.close();
        }
    });
    ThreadJoiner const pax_joiner(pax_thread);

    FrameRetentionBuffer retention(opts.retention_frame_count);
    TcpArchiverState state{opts.initial_volume_seq_num, 0, false};

    uint64_t total_frames_served = 0;

    try {
        while (!state.archive_complete) {
            int const client = accept(listener, nullptr, nullptr);
            if (client < 0) {
                int const saved_errno = errno;
                throw std::runtime_error(
                    std::format("accept: {}", std::strerror(saved_errno)));
            }
            NEOTAPE_DEBUG("archiver: accepted connection for volume seq={}\n",
                          state.next_volume_seq_num);
            FdGuard const client_guard(client);

            ServeResult result =
                serve_client(client, state, retention, frame_queue, opts,
                             archive_uuid, get_pax_error_text);
            total_frames_served += result.frames_served;

            if (result.archive_complete) {
                std::cerr << std::format(
                    "archiver: archive complete, served {} frames on this "
                    "connection\n",
                    result.frames_served);
                state.archive_complete = true;
            } else if (result.volume_committed) {
                std::cerr << std::format(
                    "archiver: connection closed, volume {} committed, "
                    "advancing to seq={}\n",
                    state.next_volume_seq_num, state.next_volume_seq_num + 1);
                ++state.next_volume_seq_num;
            } else {
                std::cerr << std::format(
                    "archiver: connection closed before commit, reusing "
                    "volume seq={}\n",
                    state.next_volume_seq_num);
            }
        }
    } catch (...) {
        cancelled.store(true);
        frame_queue.close();
        pax_thread.join();
        check_pax_error();
        throw;
    }

    cancelled.store(true);
    frame_queue.close();
    pax_thread.join();
    check_pax_error();
    return total_frames_served;
}

} // namespace neotape
