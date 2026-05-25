#include "neotape/cli.hpp"
#include "neotape/format.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_navigator.hpp"
#include "neotape/tape_writer.hpp"

#include <blake3.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace mt {

using std::format;
using std::string;
using std::string_view;
using std::vector;

namespace {

struct WriterState {
    TapeWriterOptions opts;
    TapeDevice *dev = nullptr;
    std::unique_ptr<TapeDevice> owned_dev;
    string archive_uuid;
    uint64_t volume_seq_num = 0;
    uint64_t logical_slice_seq_num = 0;
    uint64_t global_frame_seq_num = 0;
    uint64_t frame_seq_num_within_slice = 0;
    uint64_t current_slice_size = 0;
    bool slice_open = false;
    blake3_hasher slice_hasher;
};

// ====================== Helpers ==================================

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(1);
}

bool write_tape_record(TapeDevice &dev, const neotape::HeaderBytes &header,
                       const vector<uint8_t> *payload, uint32_t block_size) {
    vector<uint8_t> buf(block_size, 0);
    std::memcpy(buf.data(), header.data(), header.size());
    if (payload && !payload->empty()) {
        size_t copy =
            std::min(payload->size(), buf.size() - neotape::fixed_header_size);
        std::memcpy(buf.data() + neotape::fixed_header_size, payload->data(),
                    copy);
    }

    ssize_t n = ::write(dev.fd(), buf.data(), buf.size());
    if (n < 0) {
        if (errno == ENOSPC)
            return false;
        fail(format("write: {}", std::strerror(errno)));
    }
    if (static_cast<std::size_t>(n) != buf.size())
        return false;
    return true;
}

void handle_volume_change(WriterState &state,
                          uint64_t expected_volume_seq_num) {
    neotape::VolumePromptRequest req;
    req.archive_uuid = state.archive_uuid;
    req.expected_volume = expected_volume_seq_num;
    req.current_locator = neotape::Locator{"tape", state.opts.device};
    req.write_mode = true;

    state.dev->close();
    neotape::require_prompt_allowed(state.opts.control);

    auto result = neotape::prompt_for_volume_change(req);
    if (result.choice == neotape::VolumePromptChoice::abort)
        throw std::runtime_error("volume change aborted by user");
    if (result.choice == neotape::VolumePromptChoice::change_locator) {
        if (!result.replacement_locator ||
            result.replacement_locator->kind != "tape")
            throw std::runtime_error(
                "replacement locator must be tape:<device>");
        state.opts.device = result.replacement_locator->locator;
        state.owned_dev = std::make_unique<TapeDevice>(state.opts.device, true);
        state.dev = state.owned_dev.get();
        state.dev->configure_preferred_variable_block_mode(
            state.opts.volume_block_size, "neotape-write archive records",
            std::cerr);
        state.dev->rewind();
        return;
    }
    if (result.choice != neotape::VolumePromptChoice::continue_current)
        throw std::runtime_error("unsupported volume prompt choice");

    state.dev->reopen();
    state.dev->configure_preferred_variable_block_mode(
        state.opts.volume_block_size, "neotape-write archive records",
        std::cerr);
    state.dev->rewind();
}

// ====================== Volume + Frame Writers ===================

void write_volume_header(WriterState &state) {
    ++state.volume_seq_num;

    neotape::VolumeHeader vh;
    vh.volume_block_size = state.opts.volume_block_size;
    vh.archive_uuid = state.archive_uuid;
    vh.archive_name = state.opts.archive_name;
    vh.volume_seq_num = state.volume_seq_num;
    vh.payload_profile = state.opts.payload_profile == "pax"
                             ? neotape::PayloadProfile::pax
                             : neotape::PayloadProfile::raw;
    vh.volume_write_at_utc = neotape::utc_timestamp_now();

    auto bytes = neotape::serialize_volume_header(vh);
    while (!write_tape_record(*state.dev, bytes, nullptr,
                              state.opts.volume_block_size)) {
        handle_volume_change(state, state.volume_seq_num);
    }
    state.dev->write_filemark();
    state.slice_open = false;
}

void write_content_frame(WriterState &state, const vector<uint8_t> &payload,
                         bool end) {
    if (!state.slice_open) {
        ++state.logical_slice_seq_num;
        state.frame_seq_num_within_slice = 0;
        state.current_slice_size = 0;
        state.slice_open = true;
        blake3_hasher_init(&state.slice_hasher);
    }

    ++state.global_frame_seq_num;
    ++state.frame_seq_num_within_slice;

    neotape::Hash payload_hash =
        neotape::blake3_hash(payload.data(), payload.size());
    blake3_hasher_update(&state.slice_hasher, payload.data(), payload.size());
    state.current_slice_size += payload.size();

    neotape::Hash slice_hash{};
    if (end)
        blake3_hasher_finalize(&state.slice_hasher, slice_hash.data(),
                               slice_hash.size());

    neotape::FrameHeader fh;
    fh.volume_block_size = state.opts.volume_block_size;
    fh.archive_uuid = state.archive_uuid;
    fh.archive_name = state.opts.archive_name;
    fh.volume_seq_num = state.volume_seq_num;
    fh.logical_slice_seq_num = state.logical_slice_seq_num;
    fh.global_frame_seq_num = state.global_frame_seq_num;
    fh.frame_seq_num_within_slice = state.frame_seq_num_within_slice;
    fh.frame_payload_size = payload.size();
    fh.frame_payload_blake3 = payload_hash;
    uint16_t flags = 0;
    if (state.frame_seq_num_within_slice == 1)
        flags |= neotape::frame_flag_start;
    if (end) {
        fh.slice_content_size = state.current_slice_size;
        fh.slice_content_blake3 = slice_hash;
        flags |= neotape::frame_flag_end;
    }
    fh.flags = flags;

    auto bytes = neotape::serialize_frame_header(fh);
    while (!write_tape_record(*state.dev, bytes, &payload,
                              state.opts.volume_block_size)) {
        handle_volume_change(state, state.volume_seq_num + 1);
        bool saved_slice_open = state.slice_open;
        uint64_t saved_logical_slice_seq_num = state.logical_slice_seq_num;
        write_volume_header(state);
        state.slice_open = saved_slice_open;
        state.logical_slice_seq_num = saved_logical_slice_seq_num;
    }

    if (end) {
        state.slice_open = false;
        state.dev->write_filemark();
    }
}

void write_archive_end(WriterState &state) {
    neotape::ArchiveEndHeader ae;
    ae.volume_block_size = state.opts.volume_block_size;
    ae.archive_uuid = state.archive_uuid;
    ae.archive_name = state.opts.archive_name;
    ae.volume_seq_num = state.volume_seq_num;
    ae.last_logical_slice_seq_num = state.logical_slice_seq_num;
    ae.last_global_frame_seq_num = state.global_frame_seq_num;
    ae.created_by_implementation = "NeoTape reference writer phase6-mvp";
    ae.archive_end_at_utc = neotape::utc_timestamp_now();

    auto bytes = neotape::serialize_archive_end_header(ae);
    while (!write_tape_record(*state.dev, bytes, nullptr,
                              state.opts.volume_block_size)) {
        handle_volume_change(state, state.volume_seq_num + 1);
        write_volume_header(state);
    }
    state.dev->write_filemark();
}

void write_stream_payload(WriterState &state, FILE *input, bool split_slices) {
    size_t frame_payload_capacity =
        state.opts.volume_block_size - neotape::fixed_header_size;
    vector<uint8_t> buffer(frame_payload_capacity);
    vector<uint8_t> pending;
    bool have_pending = false;

    for (;;) {
        if (have_pending && split_slices &&
            state.current_slice_size + pending.size() >=
                state.opts.slice_size) {
            write_content_frame(state, pending, true);
            pending.clear();
            have_pending = false;
            continue;
        }

        size_t want = buffer.size();
        if (split_slices) {
            uint64_t pending_size = have_pending ? pending.size() : 0;
            uint64_t used = state.slice_open ? state.current_slice_size : 0;
            uint64_t remaining = state.opts.slice_size - used - pending_size;
            want = static_cast<size_t>(
                std::min<uint64_t>(buffer.size(), remaining));
        }

        size_t n = std::fread(buffer.data(), 1, want, input);
        if (n > 0) {
            if (have_pending) {
                write_content_frame(state, pending, false);
                pending.clear();
                have_pending = false;
            }
            pending.assign(buffer.begin(),
                           buffer.begin() + static_cast<std::ptrdiff_t>(n));
            have_pending = true;
        }
        if (n != want) {
            if (std::ferror(input))
                fail(format("read input: {}", std::strerror(errno)));
            break;
        }
    }

    if (have_pending)
        write_content_frame(state, pending, true);
}

void initialize_for_write(WriterState &state, TapeDevice &dev,
                          const TapeWriterOptions &opts) {
    state.opts = opts;
    state.dev = &dev;
    state.dev->configure_preferred_variable_block_mode(
        opts.volume_block_size, "neotape-write archive records", std::cerr);

    if (opts.init_mode) {
        dev.rewind();
    } else {
        nav::TapeNavigator nav(dev);
        auto r = nav.locate_append_position(opts.force_append
                                                ? nav::AppendPolicy::force
                                                : nav::AppendPolicy::strict);
        if (r.condition == nav::TapeCondition::blank) {
            if (!opts.init_if_blank)
                fail("tape is blank; use --init or --init-if-blank");
            dev.rewind();
        } else if (r.condition == nav::TapeCondition::has_corrupt_tail) {
            if (!opts.force_append)
                fail("previous archive has corrupt tail; use --force-append");
        }
    }

    state.archive_uuid = neotape::make_uuid_v4();
    write_volume_header(state);
}

} // anonymous namespace

// ====================== Public Entry Point =======================

void write_tape_archive(const TapeWriterOptions &opts) {
    TapeDevice dev(opts.device, true);

    WriterState state;
    initialize_for_write(state, dev, opts);

    FILE *input = stdin;
    if (opts.input != "-") {
        input = std::fopen(opts.input.c_str(), "rb");
        if (!input)
            fail(format("open {}: {}", opts.input, std::strerror(errno)));
    }

    write_stream_payload(state, input,
                         opts.payload_profile == "raw" && opts.slice_size_set);

    if (input != stdin && std::fclose(input) != 0)
        fail(format("close input: {}", std::strerror(errno)));

    write_archive_end(state);

    std::cerr << format("archive {} written to tape {}\n", state.archive_uuid,
                        opts.device);
}

void write_tape_archive_from_chunks(const TapeWriterOptions &opts,
                                    TapePayloadProducer producer) {
    TapeDevice dev(opts.device, true);
    write_tape_archive_from_chunks_to_device(dev, opts, std::move(producer));
}

void write_tape_archive_from_chunks_to_device(TapeDevice &dev,
                                              const TapeWriterOptions &opts,
                                              TapePayloadProducer producer) {
    WriterState state;
    initialize_for_write(state, dev, opts);

    producer([&](const uint8_t *data, std::size_t len, bool end_slice) {
        vector<uint8_t> payload;
        if (len > 0)
            payload.assign(data, data + len);
        write_content_frame(state, payload, end_slice);
    });

    write_archive_end(state);

    std::cerr << format("archive {} written to tape {}\n", state.archive_uuid,
                        opts.device);
}

} // namespace mt
