#include "neotape/tape.hpp"
#include "neotape/writer.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {
int target_fd = -1;
int write_calls = 0, close_calls = 0;
ssize_t write_result = -2;
int write_error = EIO;
bool interrupt_first = false, fail_close = false;

struct Device : mt::TapeDevice {
    explicit Device(int fd) : TapeDevice(fd, "test-tape", true) {}
};
struct Injection {
    Injection() {
        target_fd = ::open("/dev/null", O_WRONLY);
        REQUIRE(target_fd >= 0);
        write_calls = close_calls = 0;
        write_result = -2;
        write_error = EIO;
        interrupt_first = fail_close = false;
    }
    ~Injection() { target_fd = -1; }
};
} // namespace
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_close(int);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size) {
    if (fd != target_fd)
        return __real_write(fd, data, size);
    ++write_calls;
    if (interrupt_first && write_calls == 1) {
        errno = EINTR;
        return -1;
    }
    if (write_result == -2)
        return static_cast<ssize_t>(size);
    errno = write_error;
    return write_result;
}
extern "C" int __wrap_close(int fd) {
    if (fd != target_fd)
        return __real_close(fd);
    ++close_calls;
    int result = __real_close(fd);
    if (fail_close) {
        errno = EIO;
        return -1;
    }
    return result;
}

TEST_CASE("tape never appends another record after a short write",
          "[unit][tape]") {
    auto const count = GENERATE(0, 512, 4095);
    Injection injection;
    Device device(target_fd);
    write_result = count;
    std::array<std::byte, 4096> record{};
    REQUIRE_THROWS_AS(device.write_record(record.data(), record.size()),
                      mt::Error);
    REQUIRE(write_calls == 1);
}
TEST_CASE("tape retries only an interrupted write with no transferred bytes",
          "[unit][tape]") {
    Injection injection;
    Device device(target_fd);
    interrupt_first = true;
    std::array<std::byte, 4096> record{};
    REQUIRE_NOTHROW(device.write_record(record.data(), record.size()));
    REQUIRE(write_calls == 2);
}
TEST_CASE("tape preserves ENOSPC for volume handling", "[unit][tape]") {
    Injection injection;
    Device device(target_fd);
    write_result = -1;
    write_error = ENOSPC;
    std::array<std::byte, 4096> record{};
    try {
        device.write_record(record.data(), record.size());
        FAIL("expected ENOSPC");
    } catch (const mt::Error &error) {
        REQUIRE(error.error_code() == ENOSPC);
    }
    REQUIRE(write_calls == 1);
}
TEST_CASE("tape surfaces close failures without retrying released descriptors",
          "[unit][tape]") {
    Injection injection;
    {
        Device device(target_fd);
        fail_close = true;
        REQUIRE_THROWS_AS(device.close(), mt::Error);
        REQUIRE(device.fd() == -1);
        REQUIRE_NOTHROW(device.close());
    }
    REQUIRE(close_calls == 1);
}

TEST_CASE("tape flush errors leave the just-written record unacknowledged",
          "[unit][tape]") {
    struct DeferredFailure : mt::TapeDevice {
        DeferredFailure() : TapeDevice(-1, "deferred-error-tape", true) {}
        int writes = 0, status_calls = 0;
        void write_record(const void *, std::size_t) override { ++writes; }
        mt::Status do_status() override {
            if (++status_calls == 2)
                throw mt::Error("deferred-error-tape", "status flush", ENOSPC);
            return mt::Status(0, 0, 0, 0, 0, 0, 0);
        }
    } device;
    neotape::RecordSink sink(&device);
    std::array<std::byte, 4096> record{};
    // A success return would let the session ACK data whose flush failed.
    REQUIRE_THROWS_AS(sink.write(record), mt::Error);
    REQUIRE(device.writes == 1);
}
