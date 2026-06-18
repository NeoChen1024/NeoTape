#pragma once

#include "neotape/closable_queue.hpp"
#include "neotape/frame_builder.hpp"

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
    uint64_t frames_served = 0;
};

struct VolumeServeState {
    uint64_t next_volume_seq_num = 1;
    uint64_t last_acked_global_frame = 0;
    bool archive_complete = false;
};

struct VolumeServerOptions {
    std::string listen_address;
    uint32_t volume_block_size = 4 * 1024 * 1024;
    std::string archive_name;
    uint64_t initial_volume_seq_num = 1;
    uint64_t retention_frame_count = 256;
    std::string log_label;
    std::size_t queue_capacity = 8;
};

using VolumeProducer =
    std::function<void(const std::string &archive_uuid,
                       VolumeRecordQueue &frame_queue)>;

VolumeServeResult
serve_volume_client(int client, const VolumeServerOptions &opts,
                    const std::string &archive_uuid, VolumeServeState &state,
                    FrameRetentionBuffer &retention,
                    VolumeRecordQueue &frame_queue,
                    const std::function<std::string()> &get_error_text);

uint64_t run_volume_server(const VolumeServerOptions &opts,
                           VolumeProducer producer);

} // namespace neotape
