#pragma once

#include "neotape/pax_writer.hpp"
#include "neotape/signature.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/volume_server.hpp"

#include <cstdint>
#include <string>

namespace neotape {

struct TcpArchiverOptions {
    std::string listen_address; // "tcp://0.0.0.0:9123" or "unix:///path"
    uint32_t volume_block_size = 4 * 1024 * 1024;
    std::string archive_name;
    uint64_t initial_volume_seq_num = 1;
    uint64_t retention_frame_count = 256;
    bool fec_enabled = false;
    std::optional<SignifySecretKey> frame_signer;

    PaxWriterOptions pax;
};

// Blocks until the archive completes or an error occurs.
VolumeServerSummary run_tcp_archiver(const TcpArchiverOptions &opts);

} // namespace neotape
