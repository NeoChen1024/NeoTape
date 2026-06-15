#include "neotape/format.hpp"
#include "neotape/tape.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t record_size = 1024 * 1024;

[[noreturn]] void die(std::string_view msg) {
    std::cerr << msg << '\n';
    std::exit(1);
}

void write_frame_record(mt::TapeDevice &dev) {
    dev.rewind();
    dev.set_block_size(0);

    neotape::FrameHeader header;
    header.channel_type = neotape::ChannelType::CH_CONTENT;
    header.volume_block_size_kib = record_size / 1024;
    header.archive_uuid = "00000000-0000-4000-8000-000000000123";
    header.archive_label = "variable-block-probe";
    header.volume_seq_num = 1;
    header.global_frame_seq_num = 0;
    header.logical_slice_seq_num = 0;
    header.frame_seq_num_within_channel = 1;
    header.frame_payload_size = 1;
    header.flags = neotape::frame_flag_start | neotape::frame_flag_end;

    auto hdr_bytes = neotape::serialize_frame_header(header);
    std::vector<uint8_t> record(record_size, 0);
    std::memcpy(record.data(), hdr_bytes.data(), hdr_bytes.size());
    record[neotape::fixed_header_size] = 0xAB;

    header.frame_hash = neotape::compute_frame_hash(record.data(), record.size());
    hdr_bytes = neotape::serialize_frame_header(header);
    std::memcpy(record.data(), hdr_bytes.data(), hdr_bytes.size());

    ssize_t n = ::write(dev.fd(), record.data(), record.size());
    if (std::cmp_not_equal(n ,record.size())) {
        die(std::format("write failed n={} errno={} {}", n, errno, std::strerror(errno)));
}
    dev.write_filemark();
}

void read_with_16m_buffer(mt::TapeDevice &dev) {
    dev.rewind();
    dev.set_block_size(0);
    auto s = dev.status();
    std::cout << "status_block_size=" << s.block_size() << '\n';
    std::vector<uint8_t> buf(neotape::max_block_size);
    ssize_t const n = ::read(dev.fd(), buf.data(), buf.size());
    if (n < 0) {
        die(std::format("read failed errno={} {}", errno, std::strerror(errno)));
}
    std::cout << "read_ret=" << n << '\n';
    if (std::cmp_greater_equal(n ,neotape::fixed_header_size)) {
        auto header = neotape::parse_fixed_header(buf.data(), static_cast<std::size_t>(n));
        std::cout << "channel_type=" << neotape::channel_type_name(header.channel_type) << '\n';
        std::cout << "decoded_block_size=" << neotape::decoded_block_size(header) << '\n';
    }
}

void read_with_size(mt::TapeDevice &dev, uint32_t size) {
    dev.rewind();
    dev.set_block_size(0);
    std::vector<uint8_t> buf(size);
    ssize_t const n = ::read(dev.fd(), buf.data(), buf.size());
    if (n < 0) {
        die(std::format("read_{} failed errno={} {}", size, errno, std::strerror(errno)));
}
    std::cout << "read_" << size << "_ret=" << n << '\n';
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        die("usage: lto-variable-block-record-probe <write-frame|read-16m|read-2m|read-4m|read-8m|read-512k> /dev/nst0");
}

    mt::TapeDevice dev(argv[2], true);
    std::string const mode = argv[1];
    if (mode == "write-frame") {
        write_frame_record(dev);
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
    } else {
        die("unknown mode");
    }
    return 0;
}
