#include "neotape/common.hpp"
#include "neotape/extractor.hpp"
#include "neotape/format.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/validate.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace neotape {

namespace {

using neotape::create_listener;
using neotape::FdGuard;
using neotape::send_error;
using neotape::uint64_to_le_bytes;
using neotape::tcp::Message;
using neotape::tcp::MessageType;
using std::format;
using std::string;
using std::vector;

struct ExtractorState {
    FrameValidator validator;
    bool saw_archive_end = false;
    bool require_signed = false;
    vector<SignifyPublicKey> verify_keys;
    bool warned_signed_unverified = false;
    bool salvage = false;
    std::optional<uint64_t> output_slice_seq;
    uint64_t processed_frames = 0;
    bool fatal_error = false;
};

[[nodiscard]] bool write_output(FILE *output, const uint8_t *data,
                                std::size_t size) {
    if (size == 0) {
        return true;
    }
    size_t const written = std::fwrite(data, 1, size, output);
    if (written != size) {
        std::cerr << format("neotape-extractor: write output failed: {}\n",
                            std::strerror(errno));
        return false;
    }
    return true;
}

[[nodiscard]] bool flush_output(FILE *output) {
    if (std::fflush(output) != 0) {
        std::cerr << format("neotape-extractor: flush output failed: {}\n",
                            std::strerror(errno));
        return false;
    }
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
[[nodiscard]] bool process_frame(ExtractorState &state, const uint8_t *data,
                                 const CheckedFrame &frame, FILE *output) {
    FrameHeader const &header = frame.header;
    uint64_t const prev_slice_seq = state.validator.current_slice_seq_num;
    if (!frame.hash_ok && !state.verify_keys.empty() &&
        has_frame_flag_signed(header.flags)) {
        std::cerr << "neotape-extractor: signed frame hash mismatch\n";
        return false;
    }
    auto const signature_validation = validate_frame_signature(
        header, state.verify_keys, state.require_signed);
    if (signature_validation.error) {
        std::cerr << format("neotape-extractor: {}\n",
                            *signature_validation.error);
        return false;
    }
    if (signature_validation.status ==
            FrameSignatureStatus::signed_unverified &&
        !state.warned_signed_unverified) {
        std::cerr << "neotape-extractor: warning: signed frames are not "
                     "authenticated "
                     "because no public key is configured\n";
        state.warned_signed_unverified = true;
    }

    auto const validation =
        state.salvage ? state.validator.validate_salvage_frame(frame, data)
                      : state.validator.validate_restore_frame(frame, data);
    if (validation.status == RestoreFrameValidationStatus::fatal) {
        std::cerr << format("neotape-extractor: {}{}\n",
                            state.salvage ? "salvage skipped frame: " : "",
                            validation.message);
        return state.salvage;
    }
    if (validation.status == RestoreFrameValidationStatus::warning)
        std::cerr << format("neotape-extractor: warning: {}\n",
                            validation.message);
    if (state.validator.last_was_replay) {
        std::cerr << format(
            "neotape-extractor: suppressed replay global_seq={}\n",
            header.global_frame_seq_num);
        return true;
    }

    ++state.processed_frames;
    if (header.channel_type == ChannelType::CH_METADATA)
        return true;
    if ((!state.salvage && state.validator.saw_archive_end) ||
        (state.salvage && header.channel_type == ChannelType::ARCHIVE_END)) {
        if (!flush_output(output)) {
            return false;
        }
        state.saw_archive_end = true;
        return true;
    }

    // Preserve a useful output boundary for pipes and regular files.
    bool const slice_changed =
        state.salvage ? state.output_slice_seq.has_value() &&
                            header.slice_seq_num != *state.output_slice_seq
                      : header.slice_seq_num != prev_slice_seq;
    if (slice_changed) {
        if (!flush_output(output)) {
            return false;
        }
    }

    if (header.channel_type == ChannelType::CH_CONTENT) {
        const uint8_t *payload =
            data + static_cast<std::ptrdiff_t>(fixed_header_size);
        if (!write_output(output, payload, header.frame_payload_size)) {
            return false;
        }
        state.output_slice_seq = header.slice_seq_num;
    }

    return true;
}

// Serve one reader connection.  Returns true iff the archive was fully
// extracted (archive_end received and acked).
[[nodiscard]] bool serve_client(int client, ExtractorState &state,
                                FILE *output) {
    state.validator.begin_connection();
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
                const auto *data =
                    reinterpret_cast<const uint8_t *>(resp->payload.data());
                bool accepted = false;
                uint64_t gseq = 0;
                try {
                    CheckedFrame const frame =
                        check_frame(data, resp->payload.size());
                    gseq = frame.header.global_frame_seq_num;
                    accepted = process_frame(state, data, frame, output);
                } catch (const std::exception &error) {
                    std::cerr
                        << format("neotape-extractor: {}\n", error.what());
                }
                if (!accepted) {
                    state.fatal_error = true;
                    send_error(client, "frame validation failed");
                    return false;
                }

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
                NEOTAPE_DEBUG(
                    "extractor: received tape_eof, flushing output\n");
                if (!flush_output(output)) {
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
                std::cerr << format("neotape-extractor: reader error: {}\n",
                                    reason);
                return false;
            }
            case MessageType::next_frame:
            case MessageType::ack_frame:
            case MessageType::auth_challenge:
            case MessageType::auth_response:
                send_error(client, "unexpected request from extractor client");
                return false;
            }
        }
    } catch (const std::exception &e) {
        std::cerr << format("neotape-extractor: {}\n", e.what());
        return false;
    }
}

} // namespace

uint64_t run_tcp_extractor(const ExtractorOptions &opts) {
    int const listener = create_listener(opts.listen_address);
    FdGuard const listener_guard(listener);
    std::cerr << format("neotape-extractor: listening on {}\n",
                        opts.listen_address);

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
        std::cerr << format("neotape-extractor: writing to {}\n",
                            opts.output_path);
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
    state.require_signed = opts.require_signed;
    state.verify_keys = opts.verify_keys;
    state.salvage = opts.salvage;
    if (state.salvage) {
        std::cerr << "neotape-extractor: warning: SALVAGE MODE: output is not "
                     "fully verified; "
                     "invalid frames will be skipped\n";
    }
    uint64_t total_frames = 0;

    while (!state.saw_archive_end) {
        int const client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            int const saved_errno = errno;
            throw std::runtime_error(
                format("accept: {}", std::strerror(saved_errno)));
        }
        NEOTAPE_DEBUG("neotape-extractor: accepted reader connection\n");
        FdGuard const client_guard(client);

        bool const complete = serve_client(client, state, output);
        if (state.fatal_error) {
            throw std::runtime_error("unrecoverable frame validation failure");
        }
        if (complete) {
            total_frames = state.processed_frames;
            return total_frames;
        }

        // Connection dropped before completion.  Count the frames we
        // validated (the next expected seq minus 1), then wait for the
        // next reader to reconnect.
        total_frames = state.processed_frames;
        std::cerr << format("neotape-extractor: reader disconnected: "
                            "processed_frames={}; waiting for next reader\n",
                            total_frames);
    }

    return total_frames;
}

} // namespace neotape
