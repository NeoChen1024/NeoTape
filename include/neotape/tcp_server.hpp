#pragma once

#include "neotape/pax_writer.hpp"
#include "neotape/resume.hpp"
#include "neotape/signature.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/volume_server.hpp"

namespace neotape {

struct TcpArchiverOptions {
    // log_label and on_ack are owned by run_tcp_archiver. A resumed archive
    // sets archive_uuid and first_global_seq together with pax.first_slice.
    VolumeServerOptions server;
    // Store pax.plan_path as the archive catalog at the start of slice 0.
    bool write_plan_catalog = true;
    // Records slice starts for a later resume; not owned.
    ResumeLog *resume_log = nullptr;

    PaxWriterOptions pax;
};

// Blocks until the archive completes or an error occurs.
VolumeServerSummary run_tcp_archiver(const TcpArchiverOptions &opts);

} // namespace neotape
