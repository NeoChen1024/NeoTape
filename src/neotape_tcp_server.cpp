#include "neotape/frame_builder.hpp"
#include "neotape/pax_writer.hpp"
#include "neotape/tcp_server.hpp"
#include "neotape/volume_server.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace neotape {

namespace {

using std::string;

PaxWriterCallbacks make_server_callbacks(ContentFrameBuilder &builder,
                                         VolumeRecordQueue &queue) {
    return PaxWriterCallbacks{
        .begin_slice =
            [&](uint64_t slice_num) { builder.set_current_slice(slice_num); },
        .write_chunk =
            [&](PaxChunk chunk) {
                auto frames = builder.feed(chunk.bytes);
                for (auto &f : frames) {
                    if (!queue.push(VolumeRecord{std::move(f.record),
                                                 f.global_seq_num, false,
                                                 false})) {
                        throw std::runtime_error("frame consumer disconnected");
                    }
                }
            },
        .end_slice =
            [&](uint64_t) {
                for (auto &tail : builder.flush()) {
                    if (!queue.push(VolumeRecord{std::move(tail.record),
                                                 tail.global_seq_num, false,
                                                 false})) {
                        throw std::runtime_error("frame consumer disconnected");
                    }
                }
                if (!queue.push(VolumeRecord{{}, 0, true, false})) {
                    throw std::runtime_error("frame consumer disconnected");
                }
            },
    };
}

} // namespace

uint64_t run_tcp_archiver(const TcpArchiverOptions &opts) {
    VolumeServerOptions server_opts;
    server_opts.listen_address = opts.listen_address;
    server_opts.volume_block_size = opts.volume_block_size;
    server_opts.archive_name = opts.archive_name;
    server_opts.initial_volume_seq_num = opts.initial_volume_seq_num;
    server_opts.retention_frame_count = opts.retention_frame_count;
    server_opts.log_label = "archiver";
    server_opts.frame_signer = opts.frame_signer;

    return run_volume_server(server_opts, [&](const string &archive_uuid,
                                              VolumeRecordQueue &frame_queue) {
        ContentFrameBuilder builder(opts.volume_block_size, archive_uuid,
                                    opts.archive_name, opts.fec_enabled);
        auto callbacks = make_server_callbacks(builder, frame_queue);
        write_pax(opts.pax, std::move(callbacks));
        for (auto &tail : builder.flush()) {
            if (!frame_queue.push(VolumeRecord{std::move(tail.record),
                                               tail.global_seq_num, false,
                                               false})) {
                throw std::runtime_error("frame consumer disconnected");
            }
        }
        if (!frame_queue.push(
                VolumeRecord{{}, builder.next_global_seq_num(), false, true})) {
            throw std::runtime_error("frame consumer disconnected");
        }
    });
}

} // namespace neotape
