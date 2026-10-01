#pragma once

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

    [[nodiscard]] int error_code() const noexcept {
        return errnum_;
    }

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
// Position — MTIOCPOS result
// -----------------------------------------------------------------------

struct Position final {
    long block_no;
};

// -----------------------------------------------------------------------
// TapeDevice — RAII tape device handle
// -----------------------------------------------------------------------

class TapeDevice {
  public:
    explicit TapeDevice(std::string_view device_path, bool read_write = false);
    virtual ~TapeDevice();

    TapeDevice(const TapeDevice &) = delete;
    TapeDevice &operator=(const TapeDevice &) = delete;

    TapeDevice(TapeDevice &&) = delete;
    TapeDevice &operator=(TapeDevice &&) = delete;

    // -- accessors -----------------------------------------------------

    // fd() is virtual — see declaration near bottom of class
    [[nodiscard]] const std::string &
    device_path() const noexcept {
        return device_path_;
    }
    // Checked close; may report deferred backend errors. Never retries close.
    void close();

    // -- I/O -----------------------------------------------------------

    // Write exactly one physical record. A positive short write is an error.
    virtual void write_record(const void *data, std::size_t size);

    // -- positioning ---------------------------------------------------

    void rewind();
    void space_to_eod();

    void space_fwd(int count = 1);
    void space_bwd(int count = 1);
    void space_fwd_filemark(int count = 1);
    void space_bwd_filemark(int count = 1);
    void space_fwd_records(int count = 1);
    void space_bwd_records(int count = 1);

    void seek_block(long block_no);
    Position tell();

    // -- markers -------------------------------------------------------

    void write_filemark(int count = 1);

    // -- status queries ------------------------------------------------

    Status status();

    // fd() is virtual so test doubles can return a different fd
    [[nodiscard]] virtual int fd() const noexcept { return fd_; }

  protected:
    // Subclass constructor — skip char-device validation (for test doubles)
    TapeDevice(int fd, std::string_view path, bool read_write);

    // All MTIOCTOP operations route through this single virtual dispatch.
    virtual void do_mtop(int op, int count);

    // tell() and status() are separate ioctls (no mtop struct).
    virtual Position do_tell();
    virtual Status do_status();

  private:
    int fd_ = -1;
    std::string device_path_;
    bool read_write_ = false;
};

class SpoolTapeDevice final : public TapeDevice {
  public:
    explicit SpoolTapeDevice(const std::filesystem::path &root,
                             bool read_write = false);
    ~SpoolTapeDevice() override;

    SpoolTapeDevice(const SpoolTapeDevice &) = delete;
    SpoolTapeDevice &operator=(const SpoolTapeDevice &) = delete;
    SpoolTapeDevice(SpoolTapeDevice &&) = delete;
    SpoolTapeDevice &operator=(SpoolTapeDevice &&) = delete;

    [[nodiscard]] int fd() const noexcept override;

    void write_record(const void *data, std::size_t size) override;

  protected:
    void do_mtop(int op, int count) override;
    Position do_tell() override;
    Status do_status() override;

  private:
    std::filesystem::path root_;
    int spool_fd_ = -1;
    bool read_write_ = false;
    std::vector<std::filesystem::path> files_;
    uint64_t next_file_num_ = 0;
    uint64_t current_file_num_ = 0;
    uint64_t current_record_ = 0;
    uint32_t current_block_size_ = 0;
    bool current_is_temp_ = false;
    std::filesystem::path current_path_;

    void finalize_current_file();
};

} // namespace mt
