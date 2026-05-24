// Basic verification of TapeDevice helpers and spool backend behavior.
// Run: make test_tape && bin/test_tape

#include "neotape/tape_navigator.hpp"
#include "neotape/format.hpp"
#include "neotape/tape_ioctl.h"

#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unistd.h>
#include <utility>
#include <vector>

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

class BlockModeTapeDevice final : public mt::TapeDevice {
public:
    explicit BlockModeTapeDevice(bool fail_variable)
        : TapeDevice(-1, "/dev/test-block-mode", true),
          fail_variable_(fail_variable)
    {
    }

    const std::vector<std::pair<int, int>> &ops() const noexcept { return ops_; }

protected:
    void do_mtop(int op, int count) override {
        ops_.push_back({op, count});
        if (op == mt::MTSETBLK && count == 0 && fail_variable_)
            throw mt::Error(device_path(), "mtop", EIO);
    }

private:
    bool fail_variable_ = false;
    std::vector<std::pair<int, int>> ops_;
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

    // --- Test 0c: variable block mode is preferred and fixed mode is fallback ---
    {
        BlockModeTapeDevice dev(false);
        std::ostringstream warnings;
        auto result = dev.configure_preferred_variable_block_mode(
            1048576, "test archive", warnings);

        CHECK(result.mode == mt::TapeBlockMode::variable,
              "variable block mode preferred when MTSETBLK 0 succeeds");
        CHECK(result.block_size == 0,
              "variable block mode reports block size 0");
        CHECK((dev.ops().size() == 1 && dev.ops()[0] == std::pair<int, int>{mt::MTSETBLK, 0}),
              "variable block mode issues MTSETBLK 0 only");
        CHECK(warnings.str().empty(),
              "variable block mode success emits no warning");
    }

    {
        BlockModeTapeDevice dev(true);
        std::ostringstream warnings;
        auto result = dev.configure_preferred_variable_block_mode(
            1048576, "test archive", warnings);

        CHECK(result.mode == mt::TapeBlockMode::fixed,
              "fixed block mode fallback selected when MTSETBLK 0 fails");
        CHECK(result.block_size == 1048576,
              "fixed block fallback reports fallback block size");
        CHECK((dev.ops().size() == 2 &&
               dev.ops()[0] == std::pair<int, int>{mt::MTSETBLK, 0} &&
               dev.ops()[1] == std::pair<int, int>{mt::MTSETBLK, 1048576}),
              "fixed block fallback tries MTSETBLK 0 then fixed size");
        CHECK(warnings.str().find("falling back to fixed block mode") != std::string::npos,
              "fixed block fallback emits warning");
    }

    // --- Test 8: spool backend writes the new single-root .nts layout ---
    {
        namespace fs = std::filesystem;

        fs::path root = fs::temp_directory_path() / "neotape-spool-backend-test";
        fs::remove_all(root);
        fs::create_directories(root);

        mt::SpoolTapeDevice dev(root, true);

        auto write_record = [&](const auto &bytes) {
            ssize_t n = ::write(dev.fd(), bytes.data(), bytes.size());
            CHECK(n == static_cast<ssize_t>(bytes.size()),
                  "spool backend writes one fixed-size record");
            dev.write_filemark();
        };

        neotape::MediumHeader mh;
        mh.medium_uuid = "11111111-2222-3333-4444-555555555555";
        mh.medium_label = "spool-test";
        mh.initialized_at_utc = "2026-05-22T00:00:00Z";
        mh.medium_header_block_size = 4096;
        mh.medium_header_block_count = 1;
        mh.created_by_implementation = "test";
        write_record(neotape::serialize_medium_header(mh));

        neotape::VolumeHeader vh;
        vh.volume_block_size = 4096;
        vh.archive_uuid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        vh.archive_name = "archive";
        vh.volume_seq_num = 1;
        vh.payload_profile = neotape::PayloadProfile::raw;
        vh.volume_write_at_utc = "2026-05-22T00:00:01Z";
        write_record(neotape::serialize_volume_header(vh));

        neotape::FrameHeader fh;
        fh.volume_block_size = 4096;
        fh.archive_uuid = vh.archive_uuid;
        fh.archive_name = vh.archive_name;
        fh.volume_seq_num = 1;
        fh.logical_slice_seq_num = 1;
        fh.global_frame_seq_num = 1;
        fh.frame_seq_num_within_slice = 1;
        fh.frame_payload_size = 0;
        fh.flags = neotape::frame_flag_start | neotape::frame_flag_end;
        write_record(neotape::serialize_frame_header(fh));

        neotape::ArchiveEndHeader ae;
        ae.volume_block_size = 4096;
        ae.archive_uuid = vh.archive_uuid;
        ae.archive_name = vh.archive_name;
        ae.volume_seq_num = 1;
        ae.last_logical_slice_seq_num = 1;
        ae.last_global_frame_seq_num = 1;
        ae.created_by_implementation = "test";
        ae.archive_end_at_utc = "2026-05-22T00:00:02Z";
        write_record(neotape::serialize_archive_end_header(ae));

        CHECK(fs::exists(root / "tape-file-000000.medium-header.nts"),
              "medium header file uses .nts name");
        CHECK(fs::exists(root / "tape-file-000001.volume-header.nts"),
              "volume header file uses .nts name");
        CHECK(fs::exists(root / "tape-file-000002.slice-000001.nts"),
              "slice file uses parsed slice sequence name");
        CHECK(fs::exists(root / "tape-file-000003.archive-end.nts"),
              "archive end file uses .nts name");

        fs::remove_all(root);
    }

    fprintf(stderr, "\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
