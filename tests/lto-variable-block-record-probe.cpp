#include "neotape/format.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_navigator.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t record_size = 1024 * 1024;

[[noreturn]] void die(std::string_view msg) {
    std::cerr << msg << '\n';
    std::exit(1);
}

void write_volume_record(mt::TapeDevice &dev) {
    dev.rewind();
    dev.set_block_size(0);

    neotape::VolumeHeader vh;
    vh.volume_block_size = record_size;
    vh.archive_uuid = "00000000-0000-4000-8000-000000000123";
    vh.archive_name = "variable-block-probe";
    vh.volume_seq_num = 1;
    vh.payload_profile = neotape::PayloadProfile::raw;
    vh.volume_write_at_utc = "2026-05-21T00:00:00";

    auto header = neotape::serialize_volume_header(vh);
    std::vector<uint8_t> record(record_size, 0);
    std::memcpy(record.data(), header.data(), header.size());

    ssize_t n = ::write(dev.fd(), record.data(), record.size());
    if (n != static_cast<ssize_t>(record.size()))
        die(std::format("write failed n={} errno={} {}", n, errno, std::strerror(errno)));
    dev.write_filemark();
}

void read_with_16m_buffer(mt::TapeDevice &dev) {
    dev.rewind();
    dev.set_block_size(0);
    auto s = dev.status();
    std::cout << "status_block_size=" << s.block_size() << '\n';
    std::vector<uint8_t> buf(neotape::max_block_size);
    ssize_t n = ::read(dev.fd(), buf.data(), buf.size());
    if (n < 0)
        die(std::format("read failed errno={} {}", errno, std::strerror(errno)));
    std::cout << "read_ret=" << n << '\n';
    if (n >= static_cast<ssize_t>(neotape::fixed_header_size)) {
        auto parsed = neotape::parse_fixed_header(buf.data(), static_cast<std::size_t>(n));
        std::cout << "header_type=" << neotape::header_type_name(parsed.type) << '\n';
        if (parsed.volume)
            std::cout << "volume_block_size=" << parsed.volume->volume_block_size << '\n';
    }
}

void read_with_size(mt::TapeDevice &dev, uint32_t size) {
    dev.rewind();
    dev.set_block_size(0);
    std::vector<uint8_t> buf(size);
    ssize_t n = ::read(dev.fd(), buf.data(), buf.size());
    if (n < 0)
        die(std::format("read_{} failed errno={} {}", size, errno, std::strerror(errno)));
    std::cout << "read_" << size << "_ret=" << n << '\n';
}

void navigator_read(mt::TapeDevice &dev) {
    dev.rewind();
    dev.set_block_size(0);
    mt::nav::TapeNavigator nav(dev);
    auto parsed = nav.read_current_header();
    if (!parsed)
        die("navigator returned no header");
    std::cout << "navigator_type=" << neotape::header_type_name(parsed->type) << '\n';
    if (parsed->volume)
        std::cout << "navigator_volume_block_size=" << parsed->volume->volume_block_size << '\n';
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3)
        die("usage: lto-variable-block-record-probe <write-volume|read-16m|navigator-read> /dev/nst0");

    mt::TapeDevice dev(argv[2], true);
    std::string mode = argv[1];
    if (mode == "write-volume") {
        write_volume_record(dev);
    } else if (mode == "read-16m") {
        read_with_16m_buffer(dev);
    } else if (mode == "read-2m") {
        read_with_size(dev, 2 * 1024 * 1024);
    } else if (mode == "read-4m") {
        read_with_size(dev, 4 * 1024 * 1024);
    } else if (mode == "read-8m") {
        read_with_size(dev, 8 * 1024 * 1024);
    } else if (mode == "read-512k") {
        read_with_size(dev, 512 * 1024);
    } else if (mode == "navigator-read") {
        navigator_read(dev);
    } else {
        die("unknown mode");
    }
    return 0;
}
