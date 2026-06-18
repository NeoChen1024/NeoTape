#include "neotape/volume_server.hpp"

#include "neotape/common.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/thread_util.hpp"

#include <cerrno>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>

namespace neotape {

namespace {

using neotape::tcp::Message;
using neotape::tcp::MessageType;
using std::format;
using std::string;

int process_ack_payload(int client, const VolumeServerOptions &opts,
                        VolumeServeState &state,
                        FrameRetentionBuffer &retention,
                        bool &volume_committed,
                        std::optional<uint64_t> archive_end_seq,
                        const std::vector<std::byte> &payload) {
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
        NEOTAPE_DEBUG("{}: volume {} committed\n", opts.log_label,
                      state.next_volume_seq_num);
    }

    NEOTAPE_DEBUG("{}: ack frame global_seq={}\n", opts.log_label, g);
    state.last_acked_global_frame = g;
    retention.ack(g);
    if (archive_end_seq.has_value() && g >= *archive_end_seq) {
        NEOTAPE_DEBUG("{}: archive end acked\n", opts.log_label);
        return 1;
    }
    return 0;
}

} // namespace

VolumeServeResult
serve_volume_client(int client, const VolumeServerOptions &opts,
                    const string &archive_uuid, VolumeServeState &state,
                    FrameRetentionBuffer &retention,
                    VolumeRecordQueue &frame_queue,
                    const std::function<string()> &get_error_text) {
    bool volume_committed = false;
    uint64_t frames_served = 0;
    uint64_t next_send_seq = state.last_acked_global_frame == 0
                                 ? 1
                                 : state.last_acked_global_frame + 1;
    std::optional<uint64_t> archive_end_seq;

    auto outstanding_frames = [&]() -> uint64_t {
        return (next_send_seq - 1) - state.last_acked_global_frame;
    };

    auto drain_acks_until = [&](uint64_t limit) {
        while (outstanding_frames() >= limit) {
            auto ack = neotape::tcp::read_message(client);
            if (!ack.has_value()) {
                return -1;
            }
            if (ack->type != MessageType::ack_frame) {
                send_error(client, "send window full; expected ack_frame");
                return -1;
            }
            int const ack_result =
                process_ack_payload(client, opts, state, retention,
                                    volume_committed, archive_end_seq,
                                    ack->payload);
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
                return VolumeServeResult{true, volume_committed, frames_served};
            }

            switch (req->type) {
            case MessageType::next_frame: {
                int const window_result =
                    drain_acks_until(opts.retention_frame_count);
                if (window_result < 0) {
                    return VolumeServeResult{false, volume_committed,
                                             frames_served};
                }
                if (window_result > 0) {
                    return VolumeServeResult{true, volume_committed,
                                             frames_served};
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
                        string reason = get_error_text();
                        if (reason.empty()) {
                            reason = "frame queue closed";
                        }
                        send_error(client, reason.c_str());
                        return VolumeServeResult{false, volume_committed,
                                                 frames_served};
                    }
                    if (next->tape_eof) {
                        NEOTAPE_DEBUG("{}: sending tape_eof\n",
                                      opts.log_label);
                        neotape::tcp::write_message(
                            client, Message{MessageType::tape_eof, {}});
                        break;
                    }
                    if (next->done) {
                        int const ack_result = drain_acks_until(1);
                        if (ack_result < 0) {
                            return VolumeServeResult{false, volume_committed,
                                                     frames_served};
                        }
                        if (ack_result > 0) {
                            return VolumeServeResult{true, volume_committed,
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
                        return VolumeServeResult{false, volume_committed,
                                                 frames_served};
                    }
                    record = std::move(next->record);
                    retention.add(seq, record);
                }

                if (record.size() != opts.volume_block_size) {
                    send_error(client, "frame size mismatch");
                    return VolumeServeResult{false, volume_committed,
                                             frames_served};
                }
                patch_volume_seq_num(record, state.next_volume_seq_num);
                NEOTAPE_DEBUG("{}: sending frame global_seq={}\n",
                              opts.log_label, seq);
                neotape::tcp::write_message(
                    client,
                    Message{MessageType::frame_record, std::move(record)});
                ++frames_served;
                ++next_send_seq;
                break;
            }

            case MessageType::ack_frame: {
                int const ack_result =
                    process_ack_payload(client, opts, state, retention,
                                        volume_committed, archive_end_seq,
                                        req->payload);
                if (ack_result < 0) {
                    return VolumeServeResult{false, volume_committed,
                                             frames_served};
                }
                if (ack_result > 0) {
                    return VolumeServeResult{true, volume_committed,
                                             frames_served};
                }
                break;
            }

            case MessageType::tape_eof:
                send_error(client, "unexpected tape_eof request");
                return VolumeServeResult{false, volume_committed, frames_served};

            default:
                send_error(client, "unexpected request type");
                return VolumeServeResult{false, volume_committed, frames_served};
            }
        }
    } catch (const std::exception &e) {
        NEOTAPE_DEBUG("{}: client error: {}\n", opts.log_label, e.what());
        return VolumeServeResult{false, volume_committed, frames_served};
    } catch (...) {
        NEOTAPE_DEBUG("{}: unknown client error\n", opts.log_label);
        return VolumeServeResult{false, volume_committed, frames_served};
    }

    return VolumeServeResult{false, volume_committed, frames_served};
}

uint64_t run_volume_server(const VolumeServerOptions &opts,
                           VolumeProducer producer) {
    if (!valid_block_size(opts.volume_block_size)) {
        throw std::runtime_error("invalid volume block size");
    }

    int const listener = create_listener(opts.listen_address);
    FdGuard const listener_guard(listener);
    std::cerr << format("{} listening on {}\n", opts.log_label,
                        opts.listen_address);

    string const archive_uuid = make_uuid_v4();
    VolumeRecordQueue frame_queue(opts.queue_capacity);

    std::exception_ptr producer_error;
    string producer_error_text;
    std::mutex producer_error_mtx;

    auto capture_producer_error = [&](const string &text) {
        std::scoped_lock const lock(producer_error_mtx);
        if (!producer_error) {
            producer_error_text = text;
            producer_error = std::current_exception();
        }
    };

    auto get_error_text = [&]() -> string {
        std::scoped_lock const lock(producer_error_mtx);
        return producer_error_text;
    };

    auto check_producer_error = [&]() {
        std::scoped_lock const lock(producer_error_mtx);
        if (producer_error) {
            std::rethrow_exception(producer_error);
        }
    };

    std::thread producer_thread([&]() {
        try {
            producer(archive_uuid, frame_queue);
        } catch (const std::exception &e) {
            capture_producer_error(e.what());
            frame_queue.close();
        } catch (...) {
            capture_producer_error("unknown producer error");
            frame_queue.close();
        }
    });
    ThreadJoiner const producer_joiner(producer_thread);

    FrameRetentionBuffer retention(opts.retention_frame_count);
    VolumeServeState state{opts.initial_volume_seq_num, 0, false};
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
            NEOTAPE_DEBUG("{}: accepted connection for volume seq={}\n",
                          opts.log_label, state.next_volume_seq_num);

            VolumeServeResult const result =
                serve_volume_client(client, opts, archive_uuid, state,
                                    retention, frame_queue, get_error_text);
            total_frames_served += result.frames_served;

            if (result.archive_complete) {
                std::cerr << format(
                    "{}: archive complete, served {} frames on this "
                    "connection\n",
                    opts.log_label, result.frames_served);
                state.archive_complete = true;
            } else if (result.volume_committed) {
                std::cerr << format(
                    "{}: connection closed, volume {} committed, advancing "
                    "to seq={}\n",
                    opts.log_label, state.next_volume_seq_num,
                    state.next_volume_seq_num + 1);
                ++state.next_volume_seq_num;
            } else {
                std::cerr << format(
                    "{}: connection closed before commit, reusing volume "
                    "seq={}\n",
                    opts.log_label, state.next_volume_seq_num);
            }
        }
    } catch (...) {
        frame_queue.close();
        producer_thread.join();
        check_producer_error();
        throw;
    }

    frame_queue.close();
    producer_thread.join();
    check_producer_error();
    return total_frames_served;
}

} // namespace neotape
