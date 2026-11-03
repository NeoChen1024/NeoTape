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
    ExtractorState(const ExtractorOptions &options, FILE *metadata)
        : opts(options), metadata_output(metadata) {}
    const ExtractorOptions &opts;
    FILE *metadata_output; // null: discard ch_metadata payloads
    FrameValidator validator;
    bool saw_archive_end = false;
    bool warned_signed_unverified = false;
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
//   - payload is not written to the reconstructed output; frames whose
//     hash verified go to the metadata output when one is configured
//   - the frame is still structurally validated for sequence continuity
//
// On archive_end, sets state.saw_archive_end and the caller should return.
[[nodiscard]] bool process_frame(ExtractorState &state, const uint8_t *data,
                                 const CheckedFrame &frame, FILE *output) {
    FrameHeader const &header = frame.header;
    uint64_t const prev_slice_seq = state.validator.current_slice_seq_num;
    bool const salvage = state.opts.salvage;
    if (!frame.hash_ok && !state.opts.verify_keys.empty() &&
        has_frame_flag_signed(header.flags)) {
        std::cerr << "neotape-extractor: signed frame hash mismatch\n";
        return false;
    }
    auto const signature_validation = validate_frame_signature(
        header, state.opts.verify_keys, state.opts.require_signed);
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
        salvage ? state.validator.validate_salvage_frame(frame, data)
                : state.validator.validate_restore_frame(frame, data);
    if (validation.status == RestoreFrameValidationStatus::fatal) {
        std::cerr << format("neotape-extractor: {}{}\n",
                            salvage ? "salvage skipped frame: " : "",
                            validation.message);
        return salvage;
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
    if (header.channel_type == ChannelType::CH_METADATA) {
        if (state.metadata_output == nullptr || !frame.hash_ok)
            return true;
        return write_output(state.metadata_output,
                            data +
                                static_cast<std::ptrdiff_t>(fixed_header_size),
                            header.frame_payload_size) &&
               (!has_frame_flag_end(header.flags) ||
                flush_output(state.metadata_output));
    }
    if ((!salvage && state.validator.saw_archive_end) ||
        (salvage && header.channel_type == ChannelType::ARCHIVE_END)) {
        if (!flush_output(output)) {
            return false;
        }
        state.saw_archive_end = true;
        return true;
    }

    // Preserve a useful output boundary for pipes and regular files.
    bool const slice_changed =
        salvage ? state.output_slice_seq.has_value() &&
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
            case MessageType::error:
                std::cerr << format(
                    "neotape-extractor: reader error: {}\n",
                    error_payload_text(resp->payload, "reader reported error"));
                return false;
            case MessageType::next_frame:
            case MessageType::ack_frame:
            case MessageType::auth_challenge:
            case MessageType::auth_response:
                send_error(client, "unexpected request from extractor client");
                return false;
            }
        }
    } catch (const neotape::tcp::ConnectionError &e) {
        std::cerr << format("neotape-extractor: reader connection failed: {}\n",
                            e.what());
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
    StdioFile output = opts.output_path.empty()
                           ? StdioFile(stdout)
                           : StdioFile::open(opts.output_path, "wb");
    if (!opts.output_path.empty()) {
        std::cerr << format("neotape-extractor: writing to {}\n",
                            opts.output_path);
    }
    std::optional<StdioFile> metadata_output;
    if (!opts.metadata_output_path.empty()) {
        metadata_output.emplace(
            StdioFile::open(opts.metadata_output_path, "wb"));
    }

    ExtractorState state(opts,
                         metadata_output ? metadata_output->get() : nullptr);
    if (opts.salvage) {
        std::cerr << "neotape-extractor: warning: SALVAGE MODE: output is not "
                     "fully verified; "
                     "invalid frames will be skipped\n";
    }

    while (!state.saw_archive_end) {
        int const client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            int const saved_errno = errno;
            throw std::runtime_error(
                format("accept: {}", std::strerror(saved_errno)));
        }
        NEOTAPE_DEBUG("neotape-extractor: accepted reader connection\n");
        FdGuard const client_guard(client);

        bool const complete = serve_client(client, state, output.get());
        if (state.fatal_error) {
            throw std::runtime_error("unrecoverable frame validation failure");
        }
        if (complete) {
            break;
        }
        std::cerr << format("neotape-extractor: reader disconnected: "
                            "processed_frames={}; waiting for next reader\n",
                            state.processed_frames);
    }
    // Frames were flushed at archive_end; closing reports any error that
    // flush could not see.
    if (metadata_output) {
        metadata_output->close(opts.metadata_output_path);
    }
    output.close(opts.output_path.empty() ? "stdout" : opts.output_path);
    return state.processed_frames;
}

} // namespace neotape
