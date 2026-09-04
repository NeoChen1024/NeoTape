#pragma once

#include "neotape/closable_queue.hpp"
#include "neotape/frame_builder.hpp"
#include "neotape/signature.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
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

struct VolumeServeResult {
    bool archive_complete = false;
    bool volume_committed = false;
    uint64_t frame_transmissions = 0;
};

struct VolumeServeState {
    uint64_t next_volume_seq_num = 1;
    uint64_t last_acked_global_frame = 0;
    bool has_acked_any_frame = false;
    bool archive_complete = false;
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
