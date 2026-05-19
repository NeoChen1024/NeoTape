// Basic verification of FileBackedTapeDevice + TapeNavigator.
// Run: make test_tape && bin/test_tape

#include "tape_test_device.hpp"
#include "neotape/tape_navigator.hpp"
#include "neotape/format.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        failures++; \
    } else { \
        fprintf(stderr, "PASS: %s\n", msg); \
    } \
} while(0)

int main() {
    // --- Test 1: blank tape detection ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_1.bin", 4096);
        mt::nav::TapeNavigator nav(dev);
        auto r = nav.locate_append_position(mt::nav::AppendPolicy::strict);
        CHECK(!r.ok, "blank tape -> not ok");
        CHECK(r.condition == mt::nav::TapeCondition::blank, "blank tape -> blank condition");
    }

    // --- Test 2: write a header, read it back ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_2.bin", 4096, true);
        int fd = dev.fd();

        char buf[1024] = {};
        memcpy(buf, "NeoTape", 7);
        buf[8] = 1;  // version
        buf[9] = 3;  // HeaderType::frame

        ssize_t n = ::write(fd, buf, sizeof(buf));
        CHECK(n == 1024, "write 1024 bytes");
        dev.write_filemark();

        dev.rewind();
        char rbuf[1024] = {};
        n = ::read(fd, rbuf, sizeof(rbuf));
        CHECK(n == 1024, "read 1024 bytes");
        CHECK(memcmp(rbuf, "NeoTape", 7) == 0, "magic matches");
    }

    // --- Test 3: tell() throws on test device ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_3.bin", 4096);
        bool threw = false;
        try {
            dev.tell();
        } catch (const mt::Error &) {
            threw = true;
        }
        CHECK(threw, "tell() throws on FileBackedTapeDevice");
    }

    // --- Test 4: filemark index after writes ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_4.bin", 4096, true);
        CHECK(dev.file_count() == 0, "no files initially");

        char buf[1024] = {};
        ::write(dev.fd(), buf, sizeof(buf));
        dev.write_filemark();
        CHECK(dev.file_count() == 1, "one file after first write+fm");

        ::write(dev.fd(), buf, sizeof(buf));
        dev.write_filemark();
        CHECK(dev.file_count() == 2, "two files after second write+fm");
    }

    // --- Test 5: navigator on non-blank tape ---
    {
        mt::test::FileBackedTapeDevice dev("/tmp/tape_test_5.bin", 4096, true);

        neotape::ArchiveEndHeader ae;
        ae.archive_uuid = "test-uuid-0000-0000-000000000000";
        ae.archive_name = "test";
        ae.volume_block_size = 4096;
        auto bytes = neotape::serialize_archive_end_header(ae);
        ::write(dev.fd(), bytes.data(), bytes.size());
        dev.write_filemark();

        mt::nav::TapeNavigator nav(dev);
        auto r = nav.locate_append_position(mt::nav::AppendPolicy::strict);
        CHECK(r.ok, "non-blank with valid tail -> ok");
    }

    // --- Cleanup ---
    unlink("/tmp/tape_test_1.bin");
    unlink("/tmp/tape_test_2.bin");
    unlink("/tmp/tape_test_3.bin");
    unlink("/tmp/tape_test_4.bin");
    unlink("/tmp/tape_test_5.bin");

    fprintf(stderr, "\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
