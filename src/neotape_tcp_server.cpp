#include "neotape/closable_queue.hpp"
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/frame_builder.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/tcp_server.hpp"
#include "neotape/thread_util.hpp"

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
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>

namespace neotape {

namespace {

using neotape::tcp::Message;
using neotape::tcp::MessageType;
using neotape::build_archive_end_record;
using neotape::copy_header_to_record;
using neotape::finalize_record_hash;
using neotape::FrameRetentionBuffer;
using neotape::le64_from_bytes;
using neotape::patch_volume_seq_num;
using neotape::send_error;
using neotape::FdGuard;
using neotape::ThreadJoiner;
using neotape::create_listener;

// RetainedFrame and FrameRetentionBuffer are now shared via
// neotape/frame_builder.hpp

struct RecordOrDone {
    std::vector<std::byte> record;
    uint64_t global_seq_num = 0;
    uint64_t last_slice_seq_num = 0;
    bool tape_eof = false;
    bool done = false;
};

PaxWriterCallbacks make_server_callbacks(ContentFrameBuilder &builder,
                                         ClosableQueue<RecordOrDone> &queue) {
    return PaxWriterCallbacks{
        .begin_slice =
            [&](uint64_t slice_num) {
                builder.set_current_slice(slice_num + 1);
            },
        .write_chunk =
            [&](PaxChunk chunk) {
                auto frames = builder.feed(chunk.bytes);
                for (auto &f : frames) {
                    if (!queue.push(RecordOrDone{std::move(f.record),
                                                 f.global_seq_num, 0, false,
                                                 false})) {
                        throw std::runtime_error("frame consumer disconnected");
                    }
                }
            },
        .end_slice =
            [&](uint64_t slice_num) {
                if (auto tail = builder.flush(); tail.has_value()) {
                    if (!queue.push(RecordOrDone{std::move(tail->record),
                                                 tail->global_seq_num, 0, false,
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

    auto outstanding_frames = [&]() -> uint64_t {
        return (next_send_seq - 1) - state.last_acked_global_frame;
    };

    // Drain ACKs until outstanding frames < limit.
    // Returns -1 on error, 1 if archive_end was acked, 0 otherwise.
    auto drain_acks_until = [&](uint64_t limit) -> int {
        while (outstanding_frames() >= limit) {
            auto ack = neotape::tcp::read_message(client);
            if (!ack.has_value()) {
                return -1;
            }
            if (ack->type != MessageType::ack_frame) {
                send_error(client, "send window full; expected ack_frame");
                return -1;
            }
            if (ack->payload.size() != 8) {
                send_error(client, "ack_frame payload must be 8 bytes");
                return -1;
            }
            uint64_t const g = le64_from_bytes(ack->payload);
            uint64_t const expected = state.last_acked_global_frame + 1;
            if (g != expected) {
                send_error(client, std::format("ack {} out of order; "
                                               "expected {}",
                                               g, expected)
                                       .c_str());
                return -1;
            }

            if (!volume_committed) {
                volume_committed = true;
                NEOTAPE_DEBUG("archiver: volume {} committed\n",
                              state.next_volume_seq_num);
            }

            state.last_acked_global_frame = g;
            retention.ack(g);
            if (archive_end_seq.has_value() && g >= *archive_end_seq) {
                return 1;
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
                {
                    int const w = drain_acks_until(opts.retention_frame_count);
                    if (w < 0) {
                        return ServeResult{false, volume_committed,
                                           frames_served};
                    }
                    if (w > 0) {
                        return ServeResult{true, volume_committed,
                                           frames_served};
                    }
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
                        {
                            int const w = drain_acks_until(1);
                            if (w < 0) {
                                return ServeResult{false, volume_committed,
                                                   frames_served};
                            }
                            if (w > 0) {
                                return ServeResult{true, volume_committed,
                                                   frames_served};
                            }
                        }

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

                // ACK must be exactly next_expected = last_acked + 1.
                // Out-of-order, duplicate, and stale ACKs are all rejected.
                uint64_t const expected = state.last_acked_global_frame + 1;
                if (g != expected) {
                    send_error(client, std::format("ack {} out of order; "
                                                   "expected {}",
                                                   g, expected)
                                           .c_str());
                    return ServeResult{false, volume_committed, frames_served};
                }

                if (!volume_committed) {
                    volume_committed = true;
                    NEOTAPE_DEBUG("archiver: volume {} committed\n",
                                  state.next_volume_seq_num);
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
        ContentFrameBuilder builder(opts.volume_block_size, archive_uuid,
                                    opts.archive_name);

        constexpr uint64_t dummy_frame_count = 8;
        auto has_more_frames = [](uint64_t idx) {
            return idx < dummy_frame_count;
        };
        auto produce_record = [&](uint64_t /*idx*/) {
            const uint32_t cap = builder.payload_capacity();
            std::vector<std::byte> payload(static_cast<std::size_t>(cap));
            for (uint32_t i = 0; i < cap; ++i) {
                payload[i] = static_cast<std::byte>(static_cast<uint8_t>(i));
            }
            auto frames = builder.feed(std::span(payload));
            if (frames.empty()) {
                // Exact boundary: pending_ just filled to cap.
                // Flush to get the frame now since there is no more input
                // for this slice.
                auto f = builder.flush();
                assert(f.has_value());
                return std::move(f->record);
            }
            assert(frames.size() == 1);
            return std::move(frames[0].record);
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
    ContentFrameBuilder builder(opts.volume_block_size, archive_uuid,
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
                if (!frame_queue.push(RecordOrDone{std::move(tail->record),
                                                   tail->global_seq_num, 0,
                                                   false, false})) {
                    throw std::runtime_error("frame consumer disconnected");
                }
            }
            uint64_t const last_global_seq = builder.last_global_seq_num();
            uint64_t const last_slice_seq = builder.current_slice_seq_num();
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
