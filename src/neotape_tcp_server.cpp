#include "neotape/frame_builder.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/tcp_server.hpp"
#include "neotape/volume_server.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neotape {

namespace {

using std::string;

void push_frames(VolumeRecordQueue &queue, std::vector<BuiltFrame> frames) {
    for (auto &frame : frames) {
        if (!queue.push(VolumeRecord{std::move(frame.record),
                                     frame.global_seq_num, false, false})) {
            throw std::runtime_error("frame consumer disconnected");
        }
    }
}

PaxWriterCallbacks make_server_callbacks(FrameBuilder &builder,
                                         VolumeRecordQueue &queue) {
    return PaxWriterCallbacks{
        .begin_slice =
            [&](uint64_t slice_num) { builder.begin_channel(slice_num); },
        .write_chunk =
            [&](PaxChunk chunk) {
                push_frames(queue, builder.feed(chunk.bytes));
            },
        .end_slice =
            [&](uint64_t) {
                push_frames(queue, builder.flush());
                if (!queue.push(VolumeRecord{{}, 0, true, false})) {
                    throw std::runtime_error("frame consumer disconnected");
                }
            },
    };
}

// The plan file is the archive catalog: its bytes form the leading
// ch_metadata run of slice 0, ahead of the first planned content frame.
void write_plan_catalog(const std::filesystem::path &plan_path,
                        FrameBuilder &builder, VolumeRecordQueue &queue) {
    // The pax writer reads the same file again after this pass.
    if (!std::filesystem::is_regular_file(plan_path))
        throw std::runtime_error(
            std::format("plan {} is not a regular file", plan_path.string()));
    std::ifstream plan(plan_path, std::ios::binary);
    if (!plan)
        throw std::runtime_error(
            std::format("open plan {}: cannot read", plan_path.string()));
    builder.begin_channel(0, ChannelType::CH_METADATA);
    std::vector<std::byte> buffer(1024UL * 1024UL);
    while (plan.read(reinterpret_cast<char *>(buffer.data()),
                     static_cast<std::streamsize>(buffer.size())) ||
           plan.gcount() > 0) {
        push_frames(queue,
                    builder.feed(std::span<const std::byte>(
                        buffer.data(), static_cast<size_t>(plan.gcount()))));
    }
    if (plan.bad())
        throw std::runtime_error(
            std::format("read plan {}: I/O error", plan_path.string()));
    push_frames(queue, builder.flush());
    builder.begin_channel(0);
}

} // namespace

VolumeServerSummary run_tcp_archiver(const TcpArchiverOptions &opts) {
    VolumeServerOptions server_opts;
    server_opts.listen_address = opts.listen_address;
    server_opts.volume_block_size = opts.volume_block_size;
    server_opts.archive_name = opts.archive_name;
    server_opts.initial_volume_seq_num = opts.initial_volume_seq_num;
    server_opts.retention_frame_count = opts.retention_frame_count;
    server_opts.log_label = "neotape-archiver";
    server_opts.frame_signer = opts.frame_signer;

    return run_volume_server(server_opts, [&](const string &archive_uuid,
                                              VolumeRecordQueue &frame_queue) {
        FrameBuilder builder(opts.volume_block_size, archive_uuid,
                             opts.archive_name);
        if (opts.pax.plan_path)
            write_plan_catalog(*opts.pax.plan_path, builder, frame_queue);
        auto callbacks = make_server_callbacks(builder, frame_queue);
        write_pax(opts.pax, std::move(callbacks));
        push_frames(frame_queue, builder.flush());
        if (!frame_queue.push(
                VolumeRecord{{}, builder.next_global_seq_num(), false, true})) {
            throw std::runtime_error("frame consumer disconnected");
        }
    });
}

} // namespace neotape
