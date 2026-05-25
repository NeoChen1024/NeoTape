// Basic verification of TapeDevice helpers and spool backend behavior.
// Run: make test_tape && bin/test_tape

#include "neotape/tape_navigator.hpp"
#include "neotape/format.hpp"
#include "neotape/tape_ioctl.h"
#include "neotape/tape_writer.hpp"

#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
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

class MediumOnlyTapeDevice final : public mt::TapeDevice {
public:
    MediumOnlyTapeDevice() : TapeDevice(make_fd(), "/dev/test-medium-only", true) {
        neotape::MediumHeader mh;
        mh.medium_uuid = "11111111-2222-3333-4444-555555555555";
        mh.medium_label = "empty-medium";
        mh.initialized_at_utc = "2026-05-25T00:00:00Z";
        mh.medium_header_block_size = 4096;
        mh.medium_header_block_count = 1;
        mh.created_by_implementation = "test";

        auto bytes = neotape::serialize_medium_header(mh);
        std::vector<uint8_t> record(mh.medium_header_block_size, 0);
        std::memcpy(record.data(), bytes.data(), bytes.size());
        if (::write(fd(), record.data(), record.size()) !=
            static_cast<ssize_t>(record.size()))
            throw std::runtime_error("write medium-only test record");
        if (::lseek(fd(), 0, SEEK_SET) < 0)
            throw std::runtime_error("rewind medium-only test record");
    }

protected:
    void do_mtop(int op, int count) override {
        if (op == mt::MTEOM) {
            at_eod_ = true;
            file_no_ = 1;
            if (::lseek(fd(), 0, SEEK_END) < 0)
                throw mt::Error(device_path(), "lseek", errno);
            return;
        }
        if (op == mt::MTBSFM) {
            if (count > 1)
                throw mt::Error(device_path(), "filemark spacing", EIO);
            at_eod_ = false;
            file_no_ = 0;
            if (::lseek(fd(), 0, SEEK_SET) < 0)
                throw mt::Error(device_path(), "lseek", errno);
            return;
        }
        if (op == mt::MTREW) {
            at_eod_ = false;
            file_no_ = 0;
            if (::lseek(fd(), 0, SEEK_SET) < 0)
                throw mt::Error(device_path(), "lseek", errno);
            return;
        }
        throw mt::Error(device_path(), "mtop", ENOTSUP);
    }

    mt::Status do_status() override {
        long gstat = mt::GMT_ONLINE;
        if (file_no_ == 0 && !at_eod_)
            gstat |= mt::GMT_BOT;
        if (at_eod_)
            gstat |= mt::GMT_EOD | mt::GMT_EOF;
        return mt::Status(0, 0, 0, gstat, 0, file_no_, at_eod_ ? -1 : 0);
    }

private:
    static int make_fd() {
        char path[] = "/tmp/neotape-medium-only-test-XXXXXX";
        int fd = ::mkstemp(path);
        if (fd < 0)
            throw std::runtime_error("mkstemp medium-only test");
        ::unlink(path);
        return fd;
    }

    bool at_eod_ = false;
    int file_no_ = 0;
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

class FullTapeDevice final : public mt::TapeDevice {
public:
    FullTapeDevice() : TapeDevice(-1, "/dev/full", true) {
        fd_ = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
    }

    ~FullTapeDevice() override {
        if (fd_ >= 0)
            ::close(fd_);
    }

    int fd() const noexcept override { return fd_; }

protected:
    void do_mtop(int op, int) override {
        if (op == mt::MTSETBLK || op == mt::MTREW || op == mt::MTWEOF)
            return;
        throw mt::Error(device_path(), "mtop", ENOTSUP);
    }

private:
    int fd_ = -1;
};

class BaseFdFullTapeDevice final : public mt::TapeDevice {
public:
    BaseFdFullTapeDevice()
        : TapeDevice(::open("/dev/full", O_WRONLY | O_CLOEXEC), "/dev/full",
                     true) {
    }

protected:
    void do_mtop(int op, int) override {
        if (op == mt::MTSETBLK || op == mt::MTREW || op == mt::MTWEOF)
            return;
        throw mt::Error(device_path(), "mtop", ENOTSUP);
    }
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

    // --- Test 0d: initialized empty tape is appendable ---
    {
        MediumOnlyTapeDevice dev;
        mt::nav::TapeNavigator nav(dev);
        auto result = nav.locate_append_position();

        CHECK(result.ok && !result.last_header,
              "medium-header-only tape is accepted for first archive append");
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

    // --- Test 9: callback tape writer works with file-backed tape device ---
    {
        namespace fs = std::filesystem;

        fs::path root = fs::temp_directory_path() / "neotape-tape-callback-test";
        fs::remove_all(root);
        fs::create_directories(root);

        mt::SpoolTapeDevice dev(root, true);
        mt::TapeWriterOptions opts;
        opts.device = root.string();
        opts.archive_name = "callback";
        opts.volume_block_size = 4096;
        opts.init_mode = true;
        opts.payload_profile = "pax";
        CHECK(opts.control == neotape::ControlPolicy::auto_prompt,
              "tape writer defaults to auto control policy");

        const uint8_t payload[] = {'a', 'b', 'c'};
        mt::write_tape_archive_from_chunks_to_device(
            dev, opts, [&](mt::TapeChunkWriter writer) {
                writer(payload, sizeof(payload), false);
                writer(nullptr, 0, true);
            });

        CHECK(fs::exists(root / "tape-file-000000.volume-header.nts"),
              "callback tape writer emits volume header");
        CHECK(fs::exists(root / "tape-file-000001.slice-000001.nts"),
              "callback tape writer emits slice file");
        CHECK(fs::exists(root / "tape-file-000002.archive-end.nts"),
              "callback tape writer emits archive end");

        mt::SpoolTapeDevice reader(root, false);
        std::vector<uint8_t> record(opts.volume_block_size);
        ssize_t n = ::read(reader.fd(), record.data(), record.size());
        CHECK(n == static_cast<ssize_t>(record.size()),
              "file-backed tape reader reads volume header record");
        auto parsed = neotape::parse_fixed_header(record.data(), record.size());
        CHECK(parsed.volume.has_value(),
              "file-backed tape reader parses volume header");

        reader.space_fwd_filemark();
        n = ::read(reader.fd(), record.data(), record.size());
        CHECK(n == static_cast<ssize_t>(record.size()),
              "file-backed tape reader reads slice record");
        parsed = neotape::parse_fixed_header(record.data(), record.size());
        CHECK(parsed.frame.has_value(),
              "file-backed tape reader parses frame header");
        CHECK(parsed.frame && parsed.frame->frame_payload_size == sizeof(payload),
              "file-backed tape reader preserves frame payload size");

        reader.space_fwd_filemark();
        n = ::read(reader.fd(), record.data(), record.size());
        CHECK(n == static_cast<ssize_t>(record.size()),
              "file-backed tape reader reads archive end record");
        parsed = neotape::parse_fixed_header(record.data(), record.size());
        CHECK(parsed.archive_end.has_value(),
              "file-backed tape reader parses archive end");

        fs::remove_all(root);
    }

    // --- Test 10: writer honors --control=none on impossible write ---
    {
        FullTapeDevice dev;
        CHECK(dev.fd() >= 0, "opened /dev/full for failing writer test");

        mt::TapeWriterOptions opts;
        opts.device = "/dev/full";
        opts.archive_name = "no-prompt";
        opts.volume_block_size = 4096;
        opts.payload_profile = "pax";
        opts.init_mode = true;
        opts.control = neotape::ControlPolicy::none;

        bool threw = false;
        try {
            mt::write_tape_archive_from_chunks_to_device(
                dev, opts, [](mt::TapeChunkWriter) {});
        } catch (const std::exception &e) {
            threw = std::string(e.what()).find("volume change required") !=
                    std::string::npos;
        }
        CHECK(threw, "writer control=none fails instead of prompting");
    }

    // --- Test 11: writer closes tape fd before volume-change prompt path ---
    {
        BaseFdFullTapeDevice dev;
        CHECK(dev.fd() >= 0, "opened base fd /dev/full for close-before-prompt test");

        mt::TapeWriterOptions opts;
        opts.device = "/dev/full";
        opts.archive_name = "close-before-prompt";
        opts.volume_block_size = 4096;
        opts.payload_profile = "pax";
        opts.init_mode = true;
        opts.control = neotape::ControlPolicy::none;

        bool threw = false;
        try {
            mt::write_tape_archive_from_chunks_to_device(
                dev, opts, [](mt::TapeChunkWriter) {});
        } catch (const std::exception &e) {
            threw = std::string(e.what()).find("volume change required") !=
                    std::string::npos;
        }
        CHECK(threw, "close-before-prompt test reached volume-change path");
        CHECK(dev.fd() < 0,
              "writer closes current tape device before volume-change prompt");
    }

    fprintf(stderr, "\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
