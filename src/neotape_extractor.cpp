#include "neotape/common.hpp"
#include "neotape/extractor.hpp"
#include "neotape/format.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/validate.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace neotape {

namespace {

using neotape::tcp::Address;
using neotape::tcp::Message;
using neotape::tcp::MessageType;
using neotape::tcp::parse_address;
using std::format;
using std::string;
using std::vector;

struct FdGuard {
    int fd = -1;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    void release() { fd = -1; }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
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

struct ExtractorState {
    FrameValidator validator;
    bool saw_archive_end = false;
    vector<uint8_t> slice_payload;
};

void send_error(int client, const char *text) {
    auto payload = vector<std::byte>(reinterpret_cast<const std::byte *>(text),
                                     reinterpret_cast<const std::byte *>(text) +
                                         std::strlen(text));
    neotape::tcp::write_message(
        client, Message{MessageType::error, std::move(payload)});
}

vector<std::byte> uint64_to_le_bytes(uint64_t v) {
    vector<std::byte> out(8);
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xffU);
    }
    return out;
}

[[nodiscard]] bool flush_slice(ExtractorState &state, FILE *output) {
    if (state.slice_payload.empty()) {
        return true;
    }
    size_t const written = std::fwrite(state.slice_payload.data(), 1,
                                       state.slice_payload.size(), output);
    if (written != state.slice_payload.size()) {
        std::cerr << format("extractor: write output failed: {}\n",
                            std::strerror(errno));
        return false;
    }
    if (std::fflush(output) != 0) {
        std::cerr << format("extractor: flush output failed: {}\n",
                            std::strerror(errno));
        return false;
    }
    state.slice_payload.clear();
    return true;
}

// Returns true if the frame was processed successfully.
// Returns false only on unrecoverable errors.
//
// Metadata frames (ch_metadata) are advisory per spec:
//   - hash failures produce a warning but do not block extraction
//   - payload is not written to the reconstructed output
//   - the frame is still structurally validated for sequence continuity
//
// On archive_end, sets state.saw_archive_end and the caller should return.
[[nodiscard]] bool process_frame(ExtractorState &state,
                                 const vector<std::byte> &record,
                                 FILE *output) {
    const auto *data = reinterpret_cast<const uint8_t *>(record.data());
    const FrameHeader header = parse_fixed_header(data, record.size());

    uint64_t const prev_slice_seq = state.validator.current_slice_seq_num;

    RestoreFrameValidation const validation =
        state.validator.validate_restore_frame(header, data, record.size());
    if (validation.status == RestoreFrameValidationStatus::warning) {
        std::cerr << format("extractor: warning: {}\n", validation.message);
    } else if (validation.status == RestoreFrameValidationStatus::fatal) {
        std::cerr << format("extractor: {}\n", validation.message);
        return false;
    }

    if (header.channel_type == ChannelType::CH_METADATA) {
        return true;
    }

    if (state.validator.saw_archive_end) {
        if (!flush_slice(state, output)) {
            return false;
        }
        state.saw_archive_end = true;
        return true;
    }

    // Slice boundary: flush the previous slice's accumulated payload.
    if (header.logical_slice_seq_num != prev_slice_seq) {
        if (!flush_slice(state, output)) {
            return false;
        }
    }

    // --- accumulate content payload only ---
    if (header.channel_type == ChannelType::CH_CONTENT &&
        header.frame_payload_size > 0) {
        const uint8_t *payload =
            data + static_cast<std::ptrdiff_t>(fixed_header_size);
        state.slice_payload.insert(state.slice_payload.end(), payload,
                                   payload + header.frame_payload_size);
    }

    return true;
}

// Serve one reader connection.  Returns true iff the archive was fully
// extracted (archive_end received and acked).
[[nodiscard]] bool serve_client(int client, ExtractorState &state,
                                FILE *output) {
    try {
        for (;;) {
            NEOTAPE_DEBUG("extractor: requesting next frame\n");
            neotape::tcp::write_message(client,
                                        Message{MessageType::next_frame, {}});

            auto resp = neotape::tcp::read_message(client);
            if (!resp.has_value()) {
                NEOTAPE_DEBUG("extractor: client disconnected\n");
                return false;
            }

            switch (resp->type) {
            case MessageType::frame_record: {
                NEOTAPE_DEBUG("extractor: received frame_record\n");
                if (!process_frame(state, resp->payload, output)) {
                    send_error(client, "frame validation failed");
                    return false;
                }

                // Parse header again to get the global seq for the ack.
                const auto *data =
                    reinterpret_cast<const uint8_t *>(resp->payload.data());
                const FrameHeader header =
                    parse_fixed_header(data, resp->payload.size());
                const uint64_t gseq = header.global_frame_seq_num;

                if (state.saw_archive_end) {
                    NEOTAPE_DEBUG("extractor: ack archive_end global_seq={}\n",
                                  gseq);
                    neotape::tcp::write_message(
                        client, Message{MessageType::ack_frame,
                                        uint64_to_le_bytes(gseq)});
                    return true;
                }

                NEOTAPE_DEBUG("extractor: ack frame global_seq={}\n", gseq);
                neotape::tcp::write_message(
                    client,
                    Message{MessageType::ack_frame, uint64_to_le_bytes(gseq)});
                break;
            }
            case MessageType::tape_eof: {
                NEOTAPE_DEBUG("extractor: received tape_eof, flushing slice\n");
                if (!flush_slice(state, output)) {
                    return false;
                }
                // Reader is about to disconnect — return to accept next reader.
                return false;
            }
            case MessageType::error: {
                string reason;
                reason.reserve(resp->payload.size());
                for (std::byte const b : resp->payload) {
                    reason.push_back(static_cast<char>(b));
                }
                if (reason.empty()) {
                    reason = "reader reported error";
                }
                std::cerr << format("extractor: reader error: {}\n", reason);
                return false;
            }
            case MessageType::next_frame:
            case MessageType::ack_frame:
                send_error(client, "unexpected request from extractor client");
                return false;
            }
        }
    } catch (const std::exception &e) {
        std::cerr << format("extractor: {}\n", e.what());
        return false;
    }
}

} // namespace

uint64_t run_tcp_extractor(const ExtractorOptions &opts) {
    int const listener = create_listener(opts.listen_address);
    FdGuard const listener_guard(listener);
    std::cerr << format("extractor listening on {}\n", opts.listen_address);

    // Open output file once if a path is given; otherwise write to stdout.
    FILE *output = stdout;
    bool output_owned = false;
    if (!opts.output_path.empty()) {
        output = std::fopen(opts.output_path.c_str(), "wb");
        if (output == nullptr) {
            throw std::runtime_error(
                format("open {}: {}", opts.output_path, std::strerror(errno)));
        }
        output_owned = true;
        std::cerr << format("extractor writing to {}\n", opts.output_path);
    }

    struct OutputGuard {
        FILE *file;
        bool owned;
        ~OutputGuard() {
            if (owned && file != nullptr) {
                std::fclose(file);
            }
        }
    };
    OutputGuard const output_guard{output, output_owned};

    ExtractorState state;
    uint64_t total_frames = 0;

    while (!state.saw_archive_end) {
        int const client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            int const saved_errno = errno;
            throw std::runtime_error(
                format("accept: {}", std::strerror(saved_errno)));
        }
        NEOTAPE_DEBUG("extractor: accepted reader connection\n");
        FdGuard const client_guard(client);

        bool const complete = serve_client(client, state, output);
        if (complete) {
            total_frames = state.validator.expected_global_frame_seq == 0
                               ? 0
                               : state.validator.expected_global_frame_seq - 1;
            std::cerr << "extractor: archive extraction complete\n";
            return total_frames;
        }

        // Connection dropped before completion.  Count the frames we
        // validated (the next expected seq minus 1), then wait for the
        // next reader to reconnect.
        total_frames = state.validator.expected_global_frame_seq == 0
                           ? 0
                           : state.validator.expected_global_frame_seq - 1;
        std::cerr << format("extractor: reader disconnected after {} frames, "
                            "waiting for next reader\n",
                            total_frames);
    }

    return total_frames;
}

} // namespace neotape
