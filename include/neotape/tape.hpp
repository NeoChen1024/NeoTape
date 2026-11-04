#pragma once

#include "neotape/format.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mt {

// -----------------------------------------------------------------------
// Error — wraps errno with device/operation context
// -----------------------------------------------------------------------

class Error final : public std::runtime_error {
  public:
    Error(std::string_view device, std::string_view operation, int errnum);

    [[nodiscard]] int error_code() const noexcept { return errnum_; }

  private:
    int errnum_;
};

// -----------------------------------------------------------------------
// Status — parsed MTIOCGET snapshot
// -----------------------------------------------------------------------

class Status final {
  public:
    explicit Status(long mt_type, long mt_resid, long mt_dsreg, long mt_gstat,
                    long mt_erreg, int mt_fileno, int mt_blkno);

    [[nodiscard]] long type() const noexcept { return type_; }
    [[nodiscard]] long resid() const noexcept { return resid_; }
    [[nodiscard]] long dsreg() const noexcept { return dsreg_; }
    [[nodiscard]] long gstat() const noexcept { return gstat_; }
    [[nodiscard]] long erreg() const noexcept { return erreg_; }
    [[nodiscard]] int fileno() const noexcept { return fileno_; }
    [[nodiscard]] int blkno() const noexcept { return blkno_; }

    [[nodiscard]] bool eof() const noexcept;
    [[nodiscard]] bool bot() const noexcept;
    [[nodiscard]] bool eot() const noexcept;
    [[nodiscard]] bool eod() const noexcept;
    [[nodiscard]] bool online() const noexcept;

  private:
    long type_, resid_, dsreg_, gstat_, erreg_;
    int fileno_, blkno_;
};

// -----------------------------------------------------------------------
// MediaRecord — one read from a tape
// -----------------------------------------------------------------------

enum class RecordEvent { record, filemark, end };
struct MediaRecord {
    RecordEvent event = RecordEvent::end;
    std::vector<std::byte> record;
    // Tape files passed since the device was opened, or the spool file
    // number. A filemark carries the number of the file it ends.
    uint64_t file_num = 0;
    std::string source_name; // device path or spool file name
};

// -----------------------------------------------------------------------
// TapeDevice — RAII tape device handle
// -----------------------------------------------------------------------

class TapeDevice {
  public:
    TapeDevice(std::string_view device_path, bool read_write);
    virtual ~TapeDevice();

    TapeDevice(const TapeDevice &) = delete;
    TapeDevice &operator=(const TapeDevice &) = delete;

    TapeDevice(TapeDevice &&) = delete;
    TapeDevice &operator=(TapeDevice &&) = delete;

    // -- accessors -----------------------------------------------------

    [[nodiscard]] const std::string &device_path() const noexcept {
        return device_path_;
    }
    // Checked close; may report deferred backend errors. Never retries close.
    void close();

    // -- I/O -----------------------------------------------------------

    // Read the next physical record from the current position. A filemark
    // and the end of recorded data are events. An unreadable record is
    // skipped only once the driver confirms it has advanced past it.
    virtual MediaRecord read_record();

    // Write exactly one physical record. A positive short write is an error.
    virtual void write_record(const void *data, std::size_t size);

    // -- positioning ---------------------------------------------------

    void rewind();
    void space_to_eod();
    void space_fwd(int count = 1);
    void space_fwd_records(int count = 1);

    // -- markers -------------------------------------------------------

    void write_filemark(int count = 1);

    // -- drive control -------------------------------------------------

    // Request variable block mode so each read()/write() transfers exactly
    // one record of any size. Throws Error when the drive refuses it.
    void set_variable_block_mode();

    // -- status queries ------------------------------------------------

    Status status();

    // Virtual so spool devices and test doubles can present their own fd.
    [[nodiscard]] virtual int fd() const noexcept { return fd_; }

  protected:
    // Subclass constructor without char-device validation, for spool
    // devices and test doubles.
    TapeDevice(int fd, std::string_view path);

    // All MTIOCTOP operations route through this single virtual dispatch.
    virtual void do_mtop(int op, int count);

    // status() is a separate ioctl (no mtop struct).
    virtual Status do_status();

  private:
    int fd_ = -1;
    std::string device_path_;
    // Read-side state of a physical drive.
    std::vector<std::byte> read_buffer_;
    std::size_t read_size_ = neotape::max_block_size;
    uint64_t file_num_ = 0;
};

// Write-only tape emulation over a spool directory. Records append to a
// pending file; a filemark finalizes it as the next numbered tape file.
// Running out of space ends the volume like EOT: the record that did not
// fit is truncated away and never acknowledged.
class SpoolTapeWriter final : public TapeDevice {
  public:
    explicit SpoolTapeWriter(const std::filesystem::path &root);
    ~SpoolTapeWriter() override;

    SpoolTapeWriter(const SpoolTapeWriter &) = delete;
    SpoolTapeWriter &operator=(const SpoolTapeWriter &) = delete;
    SpoolTapeWriter(SpoolTapeWriter &&) = delete;
    SpoolTapeWriter &operator=(SpoolTapeWriter &&) = delete;

    [[nodiscard]] int fd() const noexcept override;

    void write_record(const void *data, std::size_t size) override;

  protected:
    void do_mtop(int op, int count) override;
    Status do_status() override;

  private:
    std::filesystem::path root_;
    int spool_fd_ = -1;
    // Finalized tape files present when the device was opened.
    std::vector<std::filesystem::path> files_;
    uint64_t current_file_num_ = 0;
    uint64_t current_record_ = 0;
    uint64_t current_bytes_ = 0;
    std::filesystem::path current_path_;

    void open_next_file();
    void commit_current_file();
};

// Read-only tape emulation over a spool directory: one record per block as
// framed by its header, a filemark at each file boundary, EOD after the
// last file. A partial record at the end of a file is reported and
// ignored.
class SpoolTapeReader final : public TapeDevice {
  public:
    explicit SpoolTapeReader(const std::filesystem::path &root);
    ~SpoolTapeReader() override;

    SpoolTapeReader(const SpoolTapeReader &) = delete;
    SpoolTapeReader &operator=(const SpoolTapeReader &) = delete;
    SpoolTapeReader(SpoolTapeReader &&) = delete;
    SpoolTapeReader &operator=(SpoolTapeReader &&) = delete;

    [[nodiscard]] int fd() const noexcept override;

    MediaRecord read_record() override;
    void write_record(const void *data, std::size_t size) override;

  protected:
    void do_mtop(int op, int count) override;
    Status do_status() override;

  private:
    std::vector<std::filesystem::path> files_;
    std::size_t index_ = 0;
    int spool_fd_ = -1;
    uint64_t file_num_ = 0;
    uint32_t block_size_ = 0;

    void open_file(std::size_t index);
    MediaRecord end_of_file();
};

} // namespace mt
