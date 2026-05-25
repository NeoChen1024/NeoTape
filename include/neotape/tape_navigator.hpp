#pragma once

#include "neotape/format.hpp"
#include "neotape/tape.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mt {
namespace nav {

enum class AppendPolicy {
    strict,
    inspect,
    force,
};

enum class TapeCondition {
    blank,
    has_valid_tail,
    has_corrupt_tail,
};

struct AppendResult {
    bool ok;
    TapeCondition condition;
    std::optional<neotape::ArchiveEndHeader> last_header;
};

struct ArchiveBoundary {
    uint64_t volume_fileno;
    uint64_t end_fileno;
    neotape::VolumeHeader volume_header;
    neotape::ArchiveEndHeader end_header;
    bool complete = false;
};

class TapeNavigator {
  public:
    explicit TapeNavigator(TapeDevice &dev);

    AppendResult
    locate_append_position(AppendPolicy policy = AppendPolicy::strict);
    AppendResult inspect();

    std::optional<neotape::ParsedHeader> read_current_header();

    std::vector<ArchiveBoundary> scan_archive_instances();
    bool locate_instance(uint64_t n);
    bool seek_volume(uint64_t volume_seq_num);

  private:
    TapeDevice &dev_;
};

} // namespace nav
} // namespace mt
