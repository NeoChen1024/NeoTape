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

PaxWriterCallbacks make_server_callbacks(FrameBuilder &builder,
                                         VolumeRecordQueue &queue,
                                         ResumeLog *resume_log) {
    return PaxWriterCallbacks{
        .begin_slice =
            [&builder, resume_log](uint64_t slice_num) {
                builder.begin_channel(slice_num);
                // Slice 0 is the start of the archive, not a resume point.
                if (resume_log != nullptr && slice_num > 0)
                    resume_log->slice_started(slice_num,
                                              builder.next_global_seq_num());
            },
        .write_chunk =
            [&](PaxChunk chunk) {
                push_frames(queue, builder.feed(chunk.bytes));
            },
        .end_slice =
            [&](uint64_t) {
                push_frames(queue, builder.flush());
                push_record(queue, VolumeRecord{.record = {},
                                                .global_seq_num = 0,
                                                .tape_eof = true,
                                                .done = false});
            },
    };
}

// The plan file is the archive catalog: its bytes form the leading
// ch_metadata run of slice 0, ahead of the first planned content frame.
void write_catalog(const std::filesystem::path &plan_path,
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
    VolumeServerOptions server_opts = opts.server;
    server_opts.log_label = "neotape-archiver";
    if (opts.resume_log != nullptr)
        server_opts.on_ack = [log = opts.resume_log](uint64_t seq,
                                                     uint64_t volume) {
            log->frame_acked(seq, volume);
        };

    return run_volume_server(server_opts, [&](const string &archive_uuid,
                                              VolumeRecordQueue &frame_queue) {
        FrameBuilder builder(opts.server.volume_block_size, archive_uuid,
                             opts.server.archive_name,
                             opts.server.first_global_seq);
        // A resumed run starts after slice 0, which holds the catalog.
        if (opts.pax.plan_path && opts.write_plan_catalog &&
            opts.pax.first_slice == 0)
            write_catalog(*opts.pax.plan_path, builder, frame_queue);
        auto callbacks =
            make_server_callbacks(builder, frame_queue, opts.resume_log);
        write_pax(opts.pax, std::move(callbacks));
        push_frames(frame_queue, builder.flush());
        push_record(frame_queue, VolumeRecord{.record = {},
                                              .global_seq_num =
                                                  builder.next_global_seq_num(),
                                              .tape_eof = false,
                                              .done = true});
    });
}

} // namespace neotape
