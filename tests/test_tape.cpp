// Basic verification of TapeDevice helpers and spool backend behavior.
// Run: make test_tape && bin/test_tape

#include "neotape/format.hpp"
#include "neotape/reader.hpp"
#include "neotape/tape_navigator.hpp"
#include "neotape/tape_ioctl.h"
#include "neotape/tape_writer.hpp"

#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <pty.h>
#include <stdexcept>
#include <string>
#include <sstream>
#include <sys/wait.h>
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

class ReaderSpacingTapeDevice final : public mt::TapeDevice {
public:
    explicit ReaderSpacingTapeDevice(bool start_with_medium = false)
        : TapeDevice(make_fd(), "/dev/test-reader-spacing", false),
          start_with_medium_(start_with_medium) {
        if (start_with_medium_) {
            neotape::MediumHeader mh;
            mh.medium_uuid = "11111111-2222-3333-4444-555555555555";
            mh.medium_label = "reader-spacing";
            mh.initialized_at_utc = "2026-05-25T00:00:00Z";
            mh.medium_header_block_size = 4096;
            mh.medium_header_block_count = 1;
            mh.created_by_implementation = "test";

            auto medium_bytes = neotape::serialize_medium_header(mh);
            std::vector<uint8_t> medium_record(mh.medium_header_block_size, 0);
            std::memcpy(medium_record.data(), medium_bytes.data(),
                        medium_bytes.size());
            if (::write(fd(), medium_record.data(), medium_record.size()) !=
                static_cast<ssize_t>(medium_record.size()))
                throw std::runtime_error("write reader-spacing medium header");
            volume_offset_ = static_cast<off_t>(medium_record.size());
        }

        neotape::VolumeHeader vh;
        vh.volume_block_size = 4096;
        vh.archive_uuid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        vh.archive_name = "reader-spacing";
        vh.volume_seq_num = 1;
        vh.payload_profile = neotape::PayloadProfile::pax;
        vh.volume_write_at_utc = "2026-05-25T00:00:00Z";

        auto bytes = neotape::serialize_volume_header(vh);
        std::vector<uint8_t> record(vh.volume_block_size, 0);
        std::memcpy(record.data(), bytes.data(), bytes.size());
        if (::write(fd(), record.data(), record.size()) !=
            static_cast<ssize_t>(record.size()))
            throw std::runtime_error("write reader-spacing volume header");

        slice_offset_ =
            static_cast<off_t>(start_with_medium_ ? volume_offset_ : 0) +
            static_cast<off_t>(record.size());
        neotape::FrameHeader fh;
        fh.volume_block_size = 4096;
        fh.archive_uuid = vh.archive_uuid;
        fh.archive_name = vh.archive_name;
        fh.volume_seq_num = 1;
        fh.logical_slice_seq_num = 1;
        fh.global_frame_seq_num = 1;
        fh.frame_seq_num_within_slice = 1;
        fh.flags = neotape::frame_flag_start | neotape::frame_flag_end;
        fh.frame_payload_size = 0;
        auto frame_bytes = neotape::serialize_frame_header(fh);
        std::vector<uint8_t> frame_record(vh.volume_block_size, 0);
        std::memcpy(frame_record.data(), frame_bytes.data(), frame_bytes.size());
        if (::write(fd(), frame_record.data(), frame_record.size()) !=
            static_cast<ssize_t>(frame_record.size()))
            throw std::runtime_error("write reader-spacing frame header");
        if (::lseek(fd(), 0, SEEK_SET) < 0)
            throw std::runtime_error("rewind reader-spacing volume header");
    }

    int last_op() const noexcept { return last_op_; }
    int fsf_count() const noexcept { return fsf_count_; }

protected:
    void do_mtop(int op, int) override {
        last_op_ = op;
        if (op == mt::MTFSF)
            ++fsf_count_;
        if (op == mt::MTFSF) {
            off_t target = start_with_medium_ && fsf_count_ == 1
                               ? volume_offset_
                               : slice_offset_;
            if (::lseek(fd(), target, SEEK_SET) < 0)
                throw mt::Error(device_path(), "lseek", errno);
        }
    }

private:
    static int make_fd() {
        char path[] = "/tmp/neotape-reader-spacing-test-XXXXXX";
        int fd = ::mkstemp(path);
        if (fd < 0)
            throw std::runtime_error("mkstemp reader-spacing test");
        ::unlink(path);
        return fd;
    }

    int last_op_ = -1;
    int fsf_count_ = 0;
    bool start_with_medium_ = false;
    off_t volume_offset_ = 0;
    off_t slice_offset_ = 0;
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

class RolloverTapeDevice final : public mt::TapeDevice {
public:
    RolloverTapeDevice() : TapeDevice(-1, "/dev/null", true) {
        full_fd_ = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
        if (full_fd_ < 0)
            throw std::runtime_error("open /dev/full");
        open_next_file();
    }

    ~RolloverTapeDevice() override {
        if (current_fd_ >= 0)
            ::close(current_fd_);
        if (full_fd_ >= 0)
            ::close(full_fd_);
        for (const auto &path : files_)
            ::unlink(path.c_str());
        if (!current_path_.empty())
            ::unlink(current_path_.c_str());
    }

    int fd() const noexcept override {
        if (fail_next_content_write_) {
            fail_next_content_write_ = false;
            return full_fd_;
        }
        return current_fd_;
    }

    const std::vector<std::string> &files() const noexcept { return files_; }

protected:
    void do_mtop(int op, int) override {
        if (op == mt::MTSETBLK || op == mt::MTREW)
            return;
        if (op == mt::MTWEOF) {
            finalize_current_file();
            if (files_.size() == 1)
                fail_next_content_write_ = true;
            open_next_file();
            return;
        }
        throw mt::Error(device_path(), "mtop", ENOTSUP);
    }

private:
    void open_next_file() {
        char tmpl[] = "/tmp/neotape-rollover-test-XXXXXX";
        current_fd_ = ::mkstemp(tmpl);
        if (current_fd_ < 0)
            throw std::runtime_error("mkstemp rollover test");
        current_path_ = tmpl;
    }

    void finalize_current_file() {
        if (current_fd_ >= 0) {
            if (::fsync(current_fd_) < 0)
                throw std::runtime_error("fsync rollover test");
            if (::close(current_fd_) < 0)
                throw std::runtime_error("close rollover test");
            current_fd_ = -1;
        }
        files_.push_back(current_path_);
        current_path_.clear();
    }

    int current_fd_ = -1;
    int full_fd_ = -1;
    mutable bool fail_next_content_write_ = false;
    std::string current_path_;
    std::vector<std::string> files_;
};

int run_rollover_frame_volume_child() {
    RolloverTapeDevice dev;
    mt::TapeWriterOptions opts;
    opts.device = "/dev/null";
    opts.archive_name = "rollover-frame-volume";
    opts.volume_block_size = 4096;
    opts.payload_profile = "pax";
    opts.init_mode = true;

    const uint8_t payload[] = {'x'};
    mt::write_tape_archive_from_chunks_to_device(
        dev, opts, [&](mt::TapeChunkWriter writer) {
            writer(payload, sizeof(payload), false);
            writer(nullptr, 0, true);
        });

    if (dev.files().size() < 3)
        return 2;

    std::FILE *file = std::fopen(dev.files()[2].c_str(), "rb");
    if (file == nullptr)
        return 3;
    std::vector<uint8_t> record(opts.volume_block_size);
    size_t n = std::fread(record.data(), 1, record.size(), file);
    std::fclose(file);
    if (n != record.size())
        return 4;
    auto parsed = neotape::parse_fixed_header(record.data(), record.size());
    if (!parsed.frame)
        return 5;
    return parsed.frame->volume_seq_num == 2 ? 0 : 6;
}

bool rollover_frame_volume_test_passes() {
    int master_fd = -1;
    pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, nullptr);
    if (pid < 0)
        throw std::runtime_error("forkpty rollover test");
    if (pid == 0) {
        int rc = run_rollover_frame_volume_child();
        std::_Exit(rc);
    }

    const char response[] = "c\n";
    (void)::write(master_fd, response, sizeof(response) - 1);
    char buf[256];
    while (::read(master_fd, buf, sizeof(buf)) > 0) {
    }
    ::close(master_fd);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0)
        throw std::runtime_error("waitpid rollover test");
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

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

    // --- Test 9b: navigator reports archive after volume header before clean end ---
    {
        namespace fs = std::filesystem;

        fs::path root = fs::temp_directory_path() / "neotape-tape-incomplete-list-test";
        fs::remove_all(root);
        fs::create_directories(root);

        mt::SpoolTapeDevice dev(root, true);
        neotape::VolumeHeader vh;
        vh.volume_block_size = 4096;
        vh.archive_uuid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        vh.archive_name = "incomplete";
        vh.volume_seq_num = 1;
        vh.payload_profile = neotape::PayloadProfile::pax;
        vh.volume_write_at_utc = "2026-05-25T00:00:00Z";
        auto bytes = neotape::serialize_volume_header(vh);
        ssize_t n = ::write(dev.fd(), bytes.data(), bytes.size());
        CHECK(n == static_cast<ssize_t>(bytes.size()),
              "incomplete archive test writes volume header");
        dev.write_filemark();

        mt::SpoolTapeDevice reader(root, false);
        mt::nav::TapeNavigator nav(reader);
        auto archives = nav.scan_archive_instances();
        CHECK(archives.size() == 1,
              "tape navigator lists archive after volume header before archive end");
        CHECK(!archives.empty() && !archives[0].complete,
              "tape navigator marks archive without archive end incomplete");

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

    // --- Test 10b: tape restore reader advances over the current filemark ---
    {
        ReaderSpacingTapeDevice dev;
        neotape::TapeDeviceVolumeReader reader(dev);

        CHECK(reader.next_file(),
              "tape device volume reader advances to next tape file");
        CHECK(dev.last_op() == mt::MTFSF,
              "tape device volume reader uses MTFSF to enter next file");
    }

    // --- Test 10c: tape restore reader does not skip after filemark read ---
    {
        ReaderSpacingTapeDevice dev;
        neotape::TapeDeviceVolumeReader reader(dev);
        std::vector<uint8_t> record;

        CHECK(reader.next_file(),
              "tape device volume reader enters first content file");
        CHECK(reader.read_record(record),
              "tape device volume reader reads first content record");
        CHECK(!reader.read_record(record),
              "tape device volume reader observes filemark after content record");
        int fsf_after_filemark = dev.fsf_count();
        CHECK(reader.next_file(),
              "tape device volume reader advances after consumed filemark");
        CHECK(dev.fsf_count() == fsf_after_filemark,
              "tape device volume reader does not MTFSF after filemark read");
    }

    // --- Test 10d: tape restore reader skips one leading medium header ---
    {
        ReaderSpacingTapeDevice dev(true);
        neotape::TapeDeviceVolumeReader reader(dev);

        CHECK(reader.volume_seq_num() == 1,
              "tape device volume reader finds volume header after medium header");
        CHECK(dev.last_op() == mt::MTFSF,
              "tape device volume reader skips medium header with MTFSF");
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

    // --- Test 12: writer regenerates frame headers after volume rollover ---
    {
        CHECK(rollover_frame_volume_test_passes(),
              "writer retries rolled-over frame with new volume sequence");
    }

    fprintf(stderr, "\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
