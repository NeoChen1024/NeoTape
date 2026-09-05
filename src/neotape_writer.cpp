#include "neotape/common.hpp"
#include "neotape/progress.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/validate.hpp"
#include "neotape/writer.hpp"

#include <cerrno>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <sys/socket.h>
#include <thread>

namespace neotape {

RecordSink::RecordSink(mt::TapeDevice *device, std::optional<uint64_t> capacity,
                       uint64_t used)
    : device_(device), capacity_(capacity), used_(used) {
    if (capacity_ && used_ > *capacity_)
        throw mt::Error("output", "capacity limit", ENOSPC);
}

bool RecordSink::write(std::span<const std::byte> record) {
    if (capacity_ && record.size() > *capacity_ - used_)
        throw mt::Error("output", "capacity limit", ENOSPC);
    if (device_) {
        if (device_->status().eot())
            throw mt::Error(device_->device_path(), "end of tape", ENOSPC);
        device_->write_record(record.data(), record.size());
    }
    used_ += record.size();
    if (device_) {
        try {
            return device_->status().eot();
        } catch (const mt::Error &e) {
            if (e.error_code() != ENOSPC)
                throw;
            return true;
        }
    }
    return false;
}

void RecordSink::filemark() {
    if (device_)
        device_->write_filemark();
}

namespace {
using std::format;
using tcp::Message;
using tcp::MessageType;

struct Pending {
    FrameHeader header;
    std::vector<std::byte> bytes;
    bool filemark = false;
};
enum class SessionStatus { running, complete, volume_full, failed };
struct Session {
    std::mutex mutex, socket_mutex;
    std::condition_variable cv;
    std::deque<Pending> queue;
    size_t queued_bytes = 0;
    bool input_closed = false;
    SessionStatus status = SessionStatus::running;
    std::exception_ptr error;
    uint64_t input_bytes = 0, output_bytes = 0, frames = 0;
    FrameHeader last;
};

void send(int fd, Session &state, Message message) {
    std::scoped_lock lock(state.socket_mutex);
    tcp::write_message(fd, message);
}

void output_records(int fd, RecordSink &sink, Session &state) {
    SessionStatus result = SessionStatus::failed;
    std::exception_ptr error;
    try {
        for (;;) {
            std::unique_lock lock(state.mutex);
            state.cv.wait(lock, [&] {
                return state.input_closed || !state.queue.empty();
            });
            if (state.queue.empty())
                break;
            Pending item = std::move(state.queue.front());
            state.queue.pop_front();
            state.queued_bytes -= item.bytes.size();
            state.cv.notify_all();
            lock.unlock();
            if (item.filemark) {
                sink.filemark();
                continue;
            }

            bool at_end = sink.write(item.bytes);
            {
                std::scoped_lock stats_lock(state.mutex);
                state.output_bytes += item.bytes.size();
                ++state.frames;
                state.last = item.header;
            }
            send(fd, state,
                 {MessageType::ack_frame,
                  uint64_to_le_bytes(item.header.global_frame_seq_num)});
            if (item.header.channel_type == ChannelType::ARCHIVE_END) {
                result = SessionStatus::complete;
                break;
            }
            if (at_end) {
                result = SessionStatus::volume_full;
                break;
            }
        }
    } catch (const mt::Error &e) {
        if (e.error_code() == ENOSPC)
            result = SessionStatus::volume_full;
        else
            error = std::current_exception();
    } catch (...) {
        error = std::current_exception();
    }
    {
        std::scoped_lock lock(state.mutex);
        state.status = result;
        state.error = error;
    }
    state.cv.notify_all();
    // Wake a response read when the peer is waiting for an ACK we cannot send.
    ::shutdown(fd, SHUT_RD);
}

void render_progress(Session &state, RateSampler &sampler, size_t capacity) {
    uint64_t input, output, frames, queued;
    FrameHeader last;
    {
        std::scoped_lock lock(state.mutex);
        input = state.input_bytes;
        output = state.output_bytes;
        frames = state.frames;
        queued = state.queued_bytes;
        last = state.last;
    }
    auto rates = sampler.sample(input, output, frames);
    write_progress(
        format("in @ {:>6}/s, out @ {:>6}/s, frames @ {:>6}/s, "
               "volume {:>6}, slice {:>6}, frame {:>10}, {:>6} total, buffer "
               "{:3}% full  ",
               humanize_number(rates.input), humanize_number(rates.output),
               count_rate(rates.items),
               frames ? std::to_string(last.volume_seq_num) : "-",
               frames ? std::to_string(last.slice_seq_num) : "-",
               frames ? std::to_string(last.global_frame_seq_num) : "-",
               humanize_number(output), buffer_percent(queued, capacity)));
}
} // namespace

WriteResult write_volume(int fd, RecordSink &sink,
                         const std::vector<SignifyPublicKey> &keys,
                         size_t capacity) {
    if (capacity == 0)
        throw std::invalid_argument("output buffer must not be empty");
    Session state;
    FrameValidator validator;
    bool seeded = false, warned = false;
    RateSampler sampler;
    PeriodicProgress progress(
        [&] { render_progress(state, sampler, capacity); });
    std::thread output(output_records, fd, std::ref(sink), std::ref(state));
    std::exception_ptr receive_error;
    try {
        for (;;) {
            {
                std::unique_lock lock(state.mutex);
                state.cv.wait(lock, [&] {
                    return state.status != SessionStatus::running ||
                           state.queued_bytes < capacity;
                });
                if (state.status != SessionStatus::running)
                    break;
            }
            std::optional<Message> message;
            try {
                send(fd, state, {MessageType::next_frame});
                message = tcp::read_message(fd);
            } catch (...) {
                std::scoped_lock lock(state.mutex);
                if (state.status != SessionStatus::running)
                    break;
                throw;
            }
            {
                std::scoped_lock lock(state.mutex);
                if (state.status != SessionStatus::running)
                    break;
            }
            if (!message)
                throw std::runtime_error("unexpected disconnect");
            Pending item;
            if (message->type == MessageType::tape_eof)
                item.filemark = true;
            else if (message->type == MessageType::frame_record) {
                item.bytes = std::move(message->payload);
                auto data =
                    reinterpret_cast<const uint8_t *>(item.bytes.data());
                item.header = parse_fixed_header(data, item.bytes.size());
                if (!seeded) {
                    validator.seed_for_stream_start(item.header);
                    seeded = true;
                    NEOTAPE_DEBUG(
                        "neotape-write: first frame: block_size={} "
                        "archive_label=\"{}\" volume_seq={} slice_seq={}",
                        decoded_block_size(item.header),
                        item.header.archive_label, item.header.volume_seq_num,
                        item.header.slice_seq_num);
                }
                if (auto e = validator.validate(item.header, data,
                                                item.bytes.size()))
                    throw std::runtime_error(*e);
                auto signature =
                    validate_frame_signature(item.header, keys, !keys.empty());
                if (signature.error)
                    throw std::runtime_error(*signature.error);
                if (signature.status ==
                        FrameSignatureStatus::signed_unverified &&
                    !warned) {
                    write_diagnostic("neotape-write: warning: signed frames "
                                     "are being written without authentication "
                                     "because no public key is configured");
                    warned = true;
                }
            } else if (message->type == MessageType::error) {
                throw std::runtime_error(
                    "source error: " +
                    std::string(
                        reinterpret_cast<const char *>(message->payload.data()),
                        message->payload.size()));
            } else
                throw std::runtime_error("unexpected writer response");
            bool complete = !item.filemark && item.header.channel_type ==
                                                  ChannelType::ARCHIVE_END;
            {
                std::scoped_lock lock(state.mutex);
                if (state.status != SessionStatus::running)
                    break;
                state.input_bytes += item.bytes.size();
                state.queued_bytes += item.bytes.size();
                state.queue.push_back(std::move(item));
            }
            state.cv.notify_all();
            if (complete)
                break;
        }
    } catch (...) {
        receive_error = std::current_exception();
    }
    {
        std::scoped_lock lock(state.mutex);
        state.input_closed = true;
    }
    state.cv.notify_all();
    output.join();
    progress.stop();
    if (state.error)
        std::rethrow_exception(state.error);
    if (receive_error)
        std::rethrow_exception(receive_error);
    if (state.status == SessionStatus::volume_full)
        return {WriteStatus::volume_full, state.frames,
                state.last.global_frame_seq_num};
    if (state.status != SessionStatus::complete)
        throw std::runtime_error("archive ended without archive_end");
    return {WriteStatus::complete, state.frames,
            state.last.global_frame_seq_num};
}
} // namespace neotape
