// Basic verification of FileBackedTapeDevice + TapeNavigator.
// Run: make test_tape && bin/test_tape

#include "tape_test_device.hpp"
#include "neotape/tape_navigator.hpp"
#include "neotape/format.hpp"
#include "neotape/tape_ioctl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

class BlankEodTapeDevice final : public mt::TapeDevice {
public:
    explicit BlankEodTapeDevice(bool eio_after_eod = false)
        : TapeDevice(-1, "/dev/test-blank-eod", true)
        , eio_after_eod_(eio_after_eod)
    {
    }

protected:
    void do_mtop(int op, int) override {
        if (op == mt::MTEOM) {
            at_blank_eod_ = true;
            if (eio_after_eod_)
                throw mt::Error(device_path(), "mtop", EIO);
            return;
        }
        if (op == mt::MTBSFM)
            throw mt::Error(device_path(), "mtop", EIO);
        if (op == mt::MTREW) {
            at_blank_eod_ = false;
            return;
        }
        throw mt::Error(device_path(), "mtop", ENOTSUP);
    }

    mt::Status do_status() override {
        if (at_blank_eod_)
            return mt::Status(0, 0, 0, mt::GMT_EOD | mt::GMT_ONLINE, 0, 0, -1);
        return mt::Status(0, 0, 0, mt::GMT_BOT | mt::GMT_ONLINE, 0, 0, 0);
    }

private:
    bool at_blank_eod_ = false;
    bool eio_after_eod_ = false;
};

} // namespace

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
    // --- Test 0: Linux mtio operation constants match the kernel ABI ---
    {
        CHECK(mt::MTWEOF == 5, "MTWEOF Linux ABI value");
        CHECK(mt::MTBSFM == 10, "MTBSFM Linux ABI value");
        CHECK(mt::MTFSFM == 11, "MTFSFM Linux ABI value");
        CHECK(mt::MTEOM == 12, "MTEOM Linux ABI value");
        CHECK(mt::MTERASE == 13, "MTERASE Linux ABI value");
        CHECK(mt::MTSEEK == 22, "MTSEEK Linux ABI value");
        CHECK(mt::MTTELL == 23, "MTTELL Linux ABI value");
        CHECK(mt::MTCOMPRESSION == 32, "MTCOMPRESSION Linux ABI value");
        CHECK(mt::MT_ISSCSI1 == 0x71, "MT_ISSCSI1 Linux ABI value");
        CHECK(mt::MT_ISSCSI2 == 0x72, "MT_ISSCSI2 Linux ABI value");
    }

    // --- Test 0b: observed Linux LTO density code naming ---
    {
        CHECK(mt::TapeDevice::density_name(0x58) == "LTO-5 Ultrium",
              "density code 0x58 -> LTO-5 Ultrium");
        mt::Status scsi2(0x72, 0, 0, 0, 0, 0, 0);
        CHECK(scsi2.type_name() == "SCSI 2", "type code 0x72 -> SCSI 2");
    }

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

    // --- Test 6: real blank LTO media may report EOD file 0, not BOT ---
    {
        BlankEodTapeDevice dev;
        mt::nav::TapeNavigator nav(dev);
        auto r = nav.locate_append_position(mt::nav::AppendPolicy::strict);
        CHECK(!r.ok, "blank EOD file 0 -> not ok");
        CHECK(r.condition == mt::nav::TapeCondition::blank,
              "blank EOD file 0 -> blank condition");
    }

    // --- Test 7: some drives report EIO from MTEOM after reaching blank EOD ---
    {
        BlankEodTapeDevice dev(true);
        mt::nav::TapeNavigator nav(dev);
        bool threw = false;
        mt::nav::AppendResult r;
        try {
            r = nav.locate_append_position(mt::nav::AppendPolicy::strict);
        } catch (const mt::Error &) {
            threw = true;
        }
        CHECK(!threw, "blank EOD after MTEOM EIO -> no throw");
        CHECK(!r.ok, "blank EOD after MTEOM EIO -> not ok");
        CHECK(r.condition == mt::nav::TapeCondition::blank,
              "blank EOD after MTEOM EIO -> blank condition");
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
