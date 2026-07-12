#pragma once

#include "neotape/signature.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace neotape {

struct ExtractorOptions {
    std::string listen_address; // "tcp://host:port" or "unix://path"
    std::string output_path;    // empty = stdout
    bool verbose = false;
    bool require_signed = false;
    bool salvage = false;
    std::vector<SignifyPublicKey> verify_keys;
};

// Blocks until the first reader connects, the archive is fully extracted, or
// an unrecoverable error occurs.  New readers may reconnect after a
// disconnection.  Returns the number of frames validated.
uint64_t run_tcp_extractor(const ExtractorOptions &opts);

} // namespace neotape
