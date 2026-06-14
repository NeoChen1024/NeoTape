#include "neotape/tcp_server.hpp"
#include "neotape/closable_queue.hpp"
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/tcp_protocol.hpp"

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
        if (fd < 0)
            throw std::runtime_error(
                std::format("socket: {}", std::strerror(errno)));
        unlink(a.path.c_str());
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (a.path.size() >= sizeof(sa.sun_path))
            throw std::runtime_error("unix socket path too long");
        std::memcpy(sa.sun_path, a.path.data(), a.path.size());
        if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0)
            throw std::runtime_error(
                std::format("bind {}: {}", a.path, std::strerror(errno)));
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *res = nullptr;
        int gai = getaddrinfo(a.host.c_str(), a.port.c_str(), &hints, &res);
        if (gai != 0)
            throw std::runtime_error(
                std::format("getaddrinfo: {}", gai_strerror(gai)));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res_guard(res,
                                                                     freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0)
            throw std::runtime_error(
                std::format("socket: {}", std::strerror(errno)));
        int yes = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, res->ai_addr, res->ai_addrlen) < 0)
            throw std::runtime_error(
                std::format("bind {}:{}: {}", a.host, a.port, std::strerror(errno)));
    }

    if (listen(fd, 1) < 0)
        throw std::runtime_error(
            std::format("listen: {}", std::strerror(errno)));
    return fd;
}

VolumeHeader make_volume_header(uint32_t block_size, uint64_t volume_seq_num,
                                const std::string &archive_name) {
    VolumeHeader vh;
    vh.volume_block_size = block_size;
    vh.archive_uuid = make_uuid_v4();
    vh.archive_name = archive_name;
    vh.volume_seq_num = volume_seq_num;
    vh.payload_profile = PayloadProfile::pax;
    vh.volume_write_at_utc = utc_timestamp_now();
    vh.flags = 0;
    return vh;
}

std::vector<std::byte> bytes_from_header_bytes(const HeaderBytes &bytes) {
    std::vector<std::byte> out;
    out.reserve(bytes.size());
    for (uint8_t b : bytes)
        out.push_back(static_cast<std::byte>(b));
    return out;
}

struct RetainedFrame {
    uint64_t global_seq_num = 0;
    std::vector<std::byte> record;
};

class FrameRetentionBuffer {
public:
    explicit FrameRetentionBuffer(size_t max_frames) : max_frames_(max_frames) {}

    void add(uint64_t global_seq_num, std::vector<std::byte> record) {
        if (frames_.size() == max_frames_)
            frames_.pop_front();
        frames_.push_back(RetainedFrame{global_seq_num, std::move(record)});
    }

    void ack(uint64_t global_seq_num) {
        while (!frames_.empty() && frames_.front().global_seq_num <= global_seq_num)
            frames_.pop_front();
    }

    bool has(uint64_t global_seq_num) const {
        for (const auto &f : frames_) {
            if (f.global_seq_num == global_seq_num)
                return true;
        }
        return false;
    }

    const std::vector<std::byte> *get(uint64_t global_seq_num) const {
        for (const auto &f : frames_) {
            if (f.global_seq_num == global_seq_num)
                return &f.record;
        }
        return nullptr;
    }

    uint64_t lowest_available() const {
        if (frames_.empty())
            return 0;
        return frames_.front().global_seq_num;
    }

    bool empty() const { return frames_.empty(); }

private:
    size_t max_frames_;
    std::deque<RetainedFrame> frames_;
};

struct FrameBuilder {
    uint32_t block_size;
    uint64_t volume_seq_num;
    std::string archive_uuid;
    std::string archive_name;
    uint64_t global_frame = 1;
    uint64_t slice = 0;
    std::vector<std::byte> pending;

    explicit FrameBuilder(uint32_t bs, uint64_t vol,
                          const std::string &uuid,
                          const std::string &name)
        : block_size(bs), volume_seq_num(vol), archive_uuid(uuid),
          archive_name(name) {}

    uint32_t payload_capacity() const {
        return block_size - fixed_header_size;
    }

    // Append payload bytes. Returns zero or more complete frames.
    std::vector<std::vector<std::byte>>
    feed(std::span<const std::byte> bytes,
         std::vector<uint64_t> &seq_nums) {
        std::vector<std::vector<std::byte>> out;
        pending.insert(pending.end(), bytes.begin(), bytes.end());
        const uint32_t cap = payload_capacity();
        while (pending.size() >= cap) {
            uint64_t seq = global_frame++;
            seq_nums.push_back(seq);
            out.push_back(build_frame(
                std::span(pending.begin(), cap), seq));
            pending.erase(pending.begin(), pending.begin() + cap);
        }
        return out;
    }

    // Force any remaining pending bytes into a final frame.
    std::optional<std::pair<std::vector<std::byte>, uint64_t>> flush() {
        if (pending.empty())
            return std::nullopt;
        uint64_t seq = global_frame++;
        auto rec = build_frame(std::span(pending), seq);
        pending.clear();
        return std::pair{std::move(rec), seq};
    }

    std::vector<std::byte> build_frame(std::span<const std::byte> payload,
                                       uint64_t seq_num) {
        assert(payload.size() <= payload_capacity());

        // For the first implementation, each frame is its own single-frame
        // slice.  Sequence numbers are 1-indexed.
        ++slice;

        FrameHeader fh;
        fh.volume_block_size = block_size;
        fh.archive_uuid = archive_uuid;
        fh.archive_name = archive_name;
        fh.volume_seq_num = volume_seq_num;
        fh.payload_profile = PayloadProfile::pax;
        fh.logical_slice_seq_num = slice;
        fh.global_frame_seq_num = seq_num;
        fh.frame_seq_num_within_slice = 1;
        fh.frame_payload_size = payload.size();
        fh.frame_content_type = FrameContentType::slice_content;
        fh.frame_payload_blake3 = blake3_hash(
            reinterpret_cast<const uint8_t *>(payload.data()),
            payload.size());
        fh.flags = frame_flag_start | frame_flag_end;
        fh.slice_content_size = payload.size();
        fh.slice_content_blake3 = fh.frame_payload_blake3;

        HeaderBytes header = serialize_frame_header(fh);
        std::vector<std::byte> record;
        record.reserve(block_size);
        for (uint8_t b : header)
            record.push_back(static_cast<std::byte>(b));
        record.insert(record.end(), payload.begin(), payload.end());
        record.resize(block_size); // pad with zero bytes

        return record;
    }
};

struct RecordOrDone {
    std::vector<std::byte> record;
    uint64_t global_seq_num = 0;
    uint64_t last_slice_seq_num = 0;
    bool done = false;
};

struct ThreadJoiner {
    std::thread &thread;
    explicit ThreadJoiner(std::thread &t) : thread(t) {}
    ~ThreadJoiner() {
        if (thread.joinable())
            thread.join();
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
        if (fd >= 0)
            close(fd);
    }
    void release() { fd = -1; }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
    FdGuard(FdGuard &&) = delete;
    FdGuard &operator=(FdGuard &&) = delete;
};

void send_error(int client, const char *text) {
    auto payload = std::vector<std::byte>(
        reinterpret_cast<const std::byte *>(text),
        reinterpret_cast<const std::byte *>(text) + std::strlen(text));
    neotape::tcp::write_message(client,
                                Message{MessageType::error, std::move(payload)});
}

uint64_t le64_from_bytes(const std::vector<std::byte> &payload) {
    uint64_t v = 0;
    for (std::size_t i = 0; i < payload.size() && i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(payload[i]))
             << (8 * i);
    return v;
}

PaxWriterCallbacks make_server_callbacks(FrameBuilder &builder,
                                         ClosableQueue<RecordOrDone> &queue) {
    return PaxWriterCallbacks{
        .begin_slice = [](uint64_t) {},
        .write_chunk = [&](PaxChunk chunk) {
            std::vector<uint64_t> seq_nums;
            auto frames = builder.feed(chunk.bytes, seq_nums);
            assert(frames.size() == seq_nums.size());
            for (std::size_t i = 0; i < frames.size(); ++i) {
                if (!queue.push(RecordOrDone{std::move(frames[i]), seq_nums[i],
                                             0, false}))
                    throw std::runtime_error("frame consumer disconnected");
            }
        },
        .end_slice = [](uint64_t) {},
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

ServeResult serve_client(int client, TcpArchiverState &state,
                         FrameRetentionBuffer &retention,
                         ClosableQueue<RecordOrDone> &frame_queue,
                         const TcpArchiverOptions &opts,
                         const std::string &archive_uuid,
                         const std::function<std::string()> &get_pax_error_text) {
    bool volume_committed = false;
    uint64_t frames_served = 0;
    uint64_t next_send_seq = state.last_acked_global_frame == 0
                                 ? 1
                                 : state.last_acked_global_frame + 1;
    std::vector<std::byte> vh_payload;

    auto build_volume_header_payload = [&]() {
        VolumeHeader vh = make_volume_header(opts.volume_block_size,
                                             state.next_volume_seq_num,
                                             opts.archive_name);
        vh.archive_uuid = archive_uuid;
        vh_payload = bytes_from_header_bytes(serialize_volume_header(vh));
    };

    bool archive_end_sent = false;

    try {
        for (;;) {
            auto req = neotape::tcp::read_message(client);
            if (!req.has_value())
                break;

            if (archive_end_sent && req->type != MessageType::ack_frame) {
                send_error(client, "unexpected request after archive end");
                return ServeResult{true, volume_committed, frames_served};
            }

            switch (req->type) {
            case MessageType::get_volume_header:
                NEOTAPE_DEBUG(
                    "archiver: sending volume header seq={}\n",
                    state.next_volume_seq_num);
                if (vh_payload.empty())
                    build_volume_header_payload();
                neotape::tcp::write_message(
                    client, Message{MessageType::volume_header,
                                    std::move(vh_payload)});
                build_volume_header_payload();
                break;

            case MessageType::next_frame: {
                if (!volume_committed) {
                    volume_committed = true;
                    NEOTAPE_DEBUG(
                        "archiver: volume {} committed\n",
                        state.next_volume_seq_num);
                }

                const std::vector<std::byte> *record_ptr =
                    retention.get(next_send_seq);
                uint64_t seq = 0;
                std::vector<std::byte> record;
                if (record_ptr) {
                    record = *record_ptr;
                    seq = next_send_seq;
                } else {
                    auto next = frame_queue.pop();
                    if (!next.has_value()) {
                        std::string reason = get_pax_error_text();
                        if (reason.empty())
                            reason = "frame queue closed";
                        send_error(client, reason.c_str());
                        return ServeResult{false, volume_committed,
                                           frames_served};
                    }
                    if (next->done) {
                        ArchiveEndHeader ae;
                        ae.volume_block_size = opts.volume_block_size;
                        ae.archive_uuid = archive_uuid;
                        ae.archive_name = opts.archive_name;
                        ae.volume_seq_num = state.next_volume_seq_num;
                        ae.payload_profile = PayloadProfile::pax;
                        ae.last_logical_slice_seq_num =
                            next->last_slice_seq_num;
                        ae.last_global_frame_seq_num = next->global_seq_num;
                        ae.created_by_implementation = "neotape-archiver";
                        ae.created_by_build_id = "";
                        ae.archive_end_at_utc = utc_timestamp_now();
                        ae.flags = archive_end_flag_clean_end;
                        NEOTAPE_DEBUG(
                            "archiver: sending archive end header "
                            "last_global_frame={}\n",
                            ae.last_global_frame_seq_num);
                        neotape::tcp::write_message(
                            client,
                            Message{MessageType::archive_end_header,
                                    bytes_from_header_bytes(
                                        serialize_archive_end_header(ae))});
                        NEOTAPE_DEBUG(
                            "archiver: archive end header sent, waiting for "
                            "final ack or close\n");
                        archive_end_sent = true;
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

                if (record.size() != opts.volume_block_size)
                    throw std::runtime_error("frame size mismatch");
                NEOTAPE_DEBUG(
                    "archiver: sending frame global_seq={}\n", seq);
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
                NEOTAPE_DEBUG("archiver: ack frame global_seq={}\n", g);
                state.last_acked_global_frame =
                    std::max(state.last_acked_global_frame, g);
                retention.ack(state.last_acked_global_frame);
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
    } catch (...) {
        // Do not mark the archive complete; the caller will accept the next
        // writer.
        return ServeResult{false, volume_committed, frames_served};
    }

    return ServeResult{archive_end_sent, volume_committed, frames_served};
}

} // namespace

uint64_t run_tcp_archiver(const TcpArchiverOptions &opts) {
    if (!valid_block_size(opts.volume_block_size))
        throw std::runtime_error("invalid volume block size");

    if (!opts.use_pax) {
        // Original skeleton loop using a local dummy producer.
        int listener = create_listener(opts.listen_address);
        std::cerr << std::format("archiver listening on {}\n",
                                 opts.listen_address);

        int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            int saved_errno = errno;
            close(listener);
            throw std::runtime_error(
                std::format("accept: {}", std::strerror(saved_errno)));
        }
        close(listener);

        uint64_t frames_served = 0;
        try {
            VolumeHeader vh = make_volume_header(opts.volume_block_size,
                                                 opts.initial_volume_seq_num,
                                                 opts.archive_name);
            HeaderBytes vh_bytes = serialize_volume_header(vh);
            std::vector<std::byte> vh_payload = bytes_from_header_bytes(vh_bytes);

            constexpr uint64_t dummy_frame_count = 8;
            auto has_more_frames = [](uint64_t idx) {
                return idx < dummy_frame_count;
            };
            auto produce_record = [&](uint64_t idx) {
                std::vector<std::byte> rec(opts.volume_block_size);
                for (uint32_t i = 0; i < opts.volume_block_size; ++i)
                    rec[i] = static_cast<std::byte>(
                        static_cast<uint8_t>(idx + i));
                return rec;
            };
            uint64_t request_count = 0;

            for (;;) {
                auto req = neotape::tcp::read_message(client);
                if (!req.has_value())
                    break;

                switch (req->type) {
                case MessageType::get_volume_header:
                    neotape::tcp::write_message(
                        client, Message{MessageType::volume_header,
                                        std::move(vh_payload)});
                    vh_payload = bytes_from_header_bytes(vh_bytes);
                    break;
                case MessageType::next_frame:
                    if (!has_more_frames(frames_served)) {
                        ArchiveEndHeader ae;
                        ae.volume_block_size = opts.volume_block_size;
                        ae.archive_uuid = vh.archive_uuid;
                        ae.archive_name = opts.archive_name;
                        ae.volume_seq_num = opts.initial_volume_seq_num;
                        ae.payload_profile = PayloadProfile::pax;
                        ae.last_logical_slice_seq_num = 0;
                        ae.last_global_frame_seq_num = frames_served;
                        ae.created_by_implementation = "neotape-archiver";
                        ae.created_by_build_id = "";
                        ae.archive_end_at_utc = utc_timestamp_now();
                        ae.flags = archive_end_flag_clean_end;
                        HeaderBytes ae_bytes = serialize_archive_end_header(ae);
                        neotape::tcp::write_message(
                            client,
                            Message{MessageType::archive_end_header,
                                    bytes_from_header_bytes(ae_bytes)});
                        close(client);
                        return frames_served;
                    }
                    if (request_count % 4 == 3) {
                        neotape::tcp::write_message(
                            client, Message{MessageType::tape_eof, {}});
                    } else {
                        auto rec = produce_record(frames_served);
                        if (rec.size() != opts.volume_block_size)
                            throw std::runtime_error(
                                "produce_record size mismatch");
                        neotape::tcp::write_message(
                            client,
                            Message{MessageType::frame_record,
                                    std::move(rec)});
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
        } catch (...) {
            close(client);
            throw;
        }
    }

    int listener = create_listener(opts.listen_address);
    FdGuard listener_guard(listener);
    std::cerr << std::format("archiver listening on {}\n",
                             opts.listen_address);

    std::string archive_uuid = make_uuid_v4();
    FrameBuilder builder(opts.volume_block_size, opts.initial_volume_seq_num,
                         archive_uuid, opts.archive_name);
    ClosableQueue<RecordOrDone> frame_queue(8);

    std::exception_ptr pax_error;
    std::string pax_error_text;
    std::mutex pax_error_mtx;
    std::atomic<bool> cancelled{false};

    auto capture_pax_error = [&](const std::string &text) {
        std::lock_guard lock(pax_error_mtx);
        if (!pax_error) {
            pax_error_text = text;
            pax_error = std::current_exception();
        }
    };

    auto get_pax_error_text = [&]() -> std::string {
        std::lock_guard lock(pax_error_mtx);
        return pax_error_text;
    };

    auto check_pax_error = [&]() {
        if (cancelled.load())
            return;
        std::lock_guard lock(pax_error_mtx);
        if (pax_error)
            std::rethrow_exception(pax_error);
    };

    std::thread pax_thread([&]() {
        try {
            auto callbacks = make_server_callbacks(builder, frame_queue);
            write_pax(opts.pax, std::move(callbacks));
            // Flush any trailing partial frame.
            if (auto tail = builder.flush(); tail.has_value()) {
                if (!frame_queue.push(RecordOrDone{std::move(tail->first),
                                                   tail->second, 0, false}))
                    throw std::runtime_error("frame consumer disconnected");
            }
            uint64_t last_global_seq =
                builder.global_frame == 1 ? 0 : builder.global_frame - 1;
            uint64_t last_slice_seq = builder.slice;
            if (!frame_queue.push(RecordOrDone{{}, last_global_seq,
                                               last_slice_seq, true}))
                throw std::runtime_error("frame consumer disconnected");
        } catch (const std::exception &e) {
            capture_pax_error(e.what());
            frame_queue.close();
        } catch (...) {
            capture_pax_error("unknown pax error");
            frame_queue.close();
        }
    });
    ThreadJoiner pax_joiner(pax_thread);

    FrameRetentionBuffer retention(opts.retention_frame_count);
    TcpArchiverState state{opts.initial_volume_seq_num, 0, false};

    uint64_t total_frames_served = 0;

    try {
        while (!state.archive_complete) {
            int client = accept(listener, nullptr, nullptr);
            if (client < 0) {
                int saved_errno = errno;
                throw std::runtime_error(
                    std::format("accept: {}", std::strerror(saved_errno)));
            }
            NEOTAPE_DEBUG(
                "archiver: accepted connection for volume seq={}\n",
                state.next_volume_seq_num);
            FdGuard client_guard(client);

            ServeResult result = serve_client(
                client, state, retention, frame_queue, opts, archive_uuid,
                get_pax_error_text);
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
                    state.next_volume_seq_num,
                    state.next_volume_seq_num + 1);
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
