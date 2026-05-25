#include "neotape/format.hpp"
#include "neotape/tape_navigator.hpp"

#include <unistd.h>
#include <vector>

namespace mt {
namespace nav {

namespace {

constexpr std::size_t tape_probe_read_size = 8 * 1024 * 1024;

} // namespace

TapeNavigator::TapeNavigator(TapeDevice &dev) : dev_(dev) {}

std::optional<neotape::ParsedHeader> TapeNavigator::read_current_header() {
    std::vector<uint8_t> buf(tape_probe_read_size);
    ssize_t n = ::read(dev_.fd(), buf.data(), buf.size());
    if (n <= 0)
        return std::nullopt;
    if (static_cast<std::size_t>(n) < neotape::fixed_header_size)
        buf.resize(neotape::fixed_header_size, 0);
    else
        buf.resize(static_cast<std::size_t>(n));
    return neotape::parse_fixed_header(buf.data(), buf.size());
}

AppendResult TapeNavigator::locate_append_position(AppendPolicy policy) {
    dev_.space_to_eod();

    {
        auto s = dev_.status();
        if (s.bot() || (s.eod() && s.fileno() == 0 && s.blkno() < 0))
            return {false, TapeCondition::blank, std::nullopt};
    }

    try {
        dev_.space_bwd_filemark(2);
    } catch (const Error &) {
        try {
            dev_.rewind();
            auto header = read_current_header();
            if (header && header->type == neotape::HeaderType::medium) {
                dev_.space_to_eod();
                return {true, TapeCondition::has_valid_tail, std::nullopt};
            }
        } catch (const Error &) {
        }
        return {false, TapeCondition::has_corrupt_tail, std::nullopt};
    }

    auto header = read_current_header();
    if (!header || header->type != neotape::HeaderType::archive_end) {
        if (policy == AppendPolicy::strict)
            return {false, TapeCondition::has_corrupt_tail, std::nullopt};
        dev_.space_to_eod();
        return {false, TapeCondition::has_corrupt_tail, std::nullopt};
    }

    bool crc_ok = (header->stored_crc32c == header->computed_crc32c);
    if (!crc_ok && policy == AppendPolicy::strict)
        return {false, TapeCondition::has_corrupt_tail, std::nullopt};

    dev_.space_to_eod();
    return {true, TapeCondition::has_valid_tail, header->archive_end};
}

AppendResult TapeNavigator::inspect() {
    return locate_append_position(AppendPolicy::inspect);
}

std::vector<ArchiveBoundary> TapeNavigator::scan_archive_instances() {
    std::vector<ArchiveBoundary> archives;
    dev_.rewind();

    std::optional<neotape::VolumeHeader> current_volume;
    uint64_t volume_fileno = 0;
    uint64_t current_fileno = 0;

    for (;;) {
        auto header = read_current_header();
        if (!header)
            break;

        try {
            dev_.space_fwd(1);
            current_fileno++;
        } catch (const Error &) {
            break;
        }

        if (header->type == neotape::HeaderType::volume) {
            current_volume = header->volume;
            volume_fileno = current_fileno;
        } else if (header->type == neotape::HeaderType::archive_end) {
            if (current_volume) {
                ArchiveBoundary b;
                b.volume_fileno = volume_fileno;
                b.end_fileno = current_fileno;
                b.volume_header = *current_volume;
                b.end_header = *header->archive_end;
                b.complete = true;
                archives.push_back(b);
                current_volume.reset();
            }
        }
    }

    if (current_volume) {
        ArchiveBoundary b;
        b.volume_fileno = volume_fileno;
        b.volume_header = *current_volume;
        archives.push_back(b);
    }

    return archives;
}

bool TapeNavigator::locate_instance(uint64_t n) {
    dev_.rewind();
    uint64_t found = 0;

    for (;;) {
        auto header = read_current_header();
        bool is_volume = header && header->type == neotape::HeaderType::volume;

        try {
            dev_.space_fwd(1);
        } catch (const Error &) {
            return false;
        }

        if (is_volume) {
            if (found == n) {
                dev_.space_bwd_filemark(1);
                return true;
            }
            found++;
        }
    }
}

bool TapeNavigator::seek_volume(uint64_t volume_seq_num) {
    for (;;) {
        auto header = read_current_header();
        bool matches = header && header->type == neotape::HeaderType::volume &&
                       header->volume &&
                       header->volume->volume_seq_num == volume_seq_num;

        try {
            dev_.space_fwd(1);
        } catch (const Error &) {
            return false;
        }

        if (matches) {
            dev_.space_bwd_filemark(1);
            return true;
        }
    }
}

} // namespace nav
} // namespace mt
