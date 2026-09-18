#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdarg>
#include <cstring>
#include <sys/mtio.h>

namespace {
int tape_fd = -1;
std::vector<std::vector<uint8_t>> records;
std::vector<size_t> requests;
size_t cursor = 0;
int fail_at = -1, spaces = 0;
bool failed = false, position_lost = false;
std::vector<uint8_t> record(uint32_t size, bool end = false) {
    neotape::FrameHeader header;
    header.volume_block_size_kib = size / 1024;
    header.archive_uuid = "00000000-0000-4000-8000-000000000123";
    header.flags = neotape::frame_flag_end;
    if (end) {
        header.channel_type = neotape::ChannelType::ARCHIVE_END;
        header.flags |= neotape::frame_flag_clean_end;
    }
    std::vector<uint8_t> bytes(size);
    auto encoded = neotape::serialize_frame_header(header);
    std::copy(encoded.begin(), encoded.end(), bytes.begin());
    header.frame_hash = neotape::compute_frame_hash(bytes.data(), bytes.size());
    encoded = neotape::serialize_frame_header(header);
    std::copy(encoded.begin(), encoded.end(), bytes.begin());
    return bytes;
}
} // namespace
extern "C" ssize_t __real_read(int, void *, size_t);
extern "C" int __real_ioctl(int, unsigned long, ...);
extern "C" ssize_t __wrap_read(int fd, void *data, size_t size) {
    if (fd != tape_fd)
        return __real_read(fd, data, size);
    requests.push_back(size);
    if (!failed && static_cast<int>(cursor) == fail_at) {
        failed = true;
        errno = EIO;
        return -1;
    }
    if (cursor == records.size())
        return 0;
    if (size < records[cursor].size()) {
        errno = ENOMEM;
        return -1;
    }
    auto const &bytes = records[cursor++];
    std::memcpy(data, bytes.data(), bytes.size());
    return bytes.size();
}
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list arguments;
    va_start(arguments, request);
    void *argument = va_arg(arguments, void *);
    va_end(arguments);
    if (request == MTIOCTOP && static_cast<mtop *>(argument)->mt_op == MTREW) {
        tape_fd = fd;
        return 0;
    }
    if (fd == tape_fd && request == MTIOCGET) {
        auto &status = *static_cast<mtget *>(argument);
        status = {};
        status.mt_fileno = 0;
        status.mt_blkno =
            position_lost && failed ? -1 : static_cast<int>(cursor);
        return 0;
    }
    if (fd == tape_fd && request == MTIOCTOP &&
        static_cast<mtop *>(argument)->mt_op == MTFSR) {
        ++spaces;
        ++cursor;
        return 0;
    }
    if (fd == tape_fd && request == MTIOCPOS) {
        errno = EIO;
        return -1;
    }
    return __real_ioctl(fd, request, argument);
}

TEST_CASE("tape reader uses verified record size and resets at archive end",
          "[unit][tape]") {
    records = {record(4096), record(4096), record(4096, true), record(8192)};
    cursor = 0;
    fail_at = -1;
    failed = position_lost = false;
    spaces = 0;
    requests.clear();
    neotape::RecordReader reader({neotape::MediaLocator::tape, "/dev/null"});
    for (auto const &expected : records) {
        auto result = reader.next();
        REQUIRE(result.event == neotape::RecordEvent::record);
        REQUIRE(result.record.size() == expected.size());
    }
    REQUIRE(requests == std::vector<size_t>{neotape::max_block_size, 4096, 4096,
                                            neotape::max_block_size});
}

TEST_CASE("tape skips unreadable records only with confirmed driver position",
          "[unit][tape]") {
    position_lost = GENERATE(false, true);
    records = {record(4096), record(4096), record(4096)};
    cursor = 0;
    fail_at = 1;
    failed = false;
    spaces = 0;
    requests.clear();
    neotape::RecordReader reader({neotape::MediaLocator::tape, "/dev/null"});
    REQUIRE(reader.next().event == neotape::RecordEvent::record);
    if (position_lost) {
        REQUIRE_THROWS_AS(reader.next(), mt::Error);
        REQUIRE(spaces == 0);
    } else {
        REQUIRE(reader.next().event == neotape::RecordEvent::record);
        REQUIRE(cursor == 3);
        REQUIRE(spaces == 1);
    }
}
