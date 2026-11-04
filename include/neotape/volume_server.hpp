#pragma once

#include "neotape/closable_queue.hpp"
#include "neotape/frame_builder.hpp"
#include "neotape/signature.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neotape {

struct VolumeRecord {
    std::vector<std::byte> record;
    uint64_t global_seq_num = 0;
    bool tape_eof = false;
    bool done = false;
};

using VolumeRecordQueue = ClosableQueue<VolumeRecord>;

// Producer side: hand records to the server. Throws once the server has
// stopped consuming.
void push_record(VolumeRecordQueue &queue, VolumeRecord record);
void push_frames(VolumeRecordQueue &queue, std::vector<BuiltFrame> frames);

struct VolumeServeResult {
    bool archive_complete = false;
    bool volume_committed = false;
    uint64_t frame_transmissions = 0;
    // Why the connection ended early, when it did so with an error. A
    // writer that has filled its volume may drop the connection while a
    // frame is in flight; that is only worth reporting before a commit.
    std::string disconnect_reason;
};

struct VolumeServeState {
    uint64_t next_volume_seq_num = 1;
    uint64_t last_acked_global_frame = 0;
    bool has_acked_any_frame = false;
    bool archive_complete = false;
    // Global sequence number of the first frame this run produces.
    uint64_t first_global_seq = 0;

    [[nodiscard]] uint64_t next_unacked_global_frame() const {
        return has_acked_any_frame ? last_acked_global_frame + 1
                                   : first_global_seq;
    }
};

struct VolumeServerSummary {
    uint64_t committed_frames = 0;
    uint64_t frame_transmissions = 0;
    uint64_t committed_volumes = 0;
    uint64_t connections = 0;
    uint64_t uncommitted_disconnects = 0;
};

struct VolumeServerOptions {
    std::string listen_address;
    uint32_t volume_block_size = 4 * 1024 * 1024;
    std::string archive_name;
    uint64_t initial_volume_seq_num = 1;
    uint64_t retention_frame_count = 256;
    std::string log_label;
    std::size_t queue_capacity = 8;
    std::optional<SignifySecretKey> frame_signer;
    // Resuming an archive: reuse its UUID and continue its sequence numbers.
    std::optional<std::string> archive_uuid;
    uint64_t first_global_seq = 0;
    // Called for each acknowledged frame with the volume it was written to.
    std::function<void(uint64_t global_seq, uint64_t volume_seq_num)> on_ack;
};

using VolumeProducer = std::function<void(const std::string &archive_uuid,
                                          VolumeRecordQueue &frame_queue)>;

VolumeServeResult
serve_volume_client(int client, const VolumeServerOptions &opts,
                    const std::string &archive_uuid, VolumeServeState &state,
                    FrameRetentionBuffer &retention,
                    VolumeRecordQueue &frame_queue,
                    const std::function<std::string()> &get_error_text);

VolumeServerSummary run_volume_server(const VolumeServerOptions &opts,
                                      VolumeProducer producer);

} // namespace neotape
