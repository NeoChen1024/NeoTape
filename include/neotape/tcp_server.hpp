#pragma once

#include "neotape/pax_writer.hpp"
#include "neotape/tcp_protocol.hpp"

#include <cstdint>
#include <string>

namespace neotape {

struct TcpArchiverOptions {
    std::string listen_address; // "tcp://0.0.0.0:9123" or "unix:///path"
    uint32_t volume_block_size = 4 * 1024 * 1024;
    std::string archive_name;
    uint64_t initial_volume_seq_num = 1;

    // Pax generation options when in archive mode.
    PaxWriterOptions pax;
    bool use_pax = false;
};

// Blocks until the first writer connects, the archive completes, or an error
// occurs. Returns the number of frames served on this connection.
uint64_t run_tcp_archiver(const TcpArchiverOptions &opts);

} // namespace neotape
