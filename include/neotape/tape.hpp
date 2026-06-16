#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mt {

// -----------------------------------------------------------------------
// Error — wraps errno with device/operation context
// -----------------------------------------------------------------------

class Error final : public std::runtime_error {
  public:
    Error(std::string_view device, std::string_view operation, int errnum);

    [[nodiscard]] [[nodiscard]] int error_code() const noexcept {
        return errnum_;
    }

    [[nodiscard]] [[nodiscard]] int mt_resid() const noexcept {
        return mt_resid_;
    }
    void set_mt_resid(int r) noexcept { mt_resid_ = r; }

  private:
    int errnum_;
    int mt_resid_ = 0;
};

// -----------------------------------------------------------------------
// Status — parsed MTIOCGET snapshot
// -----------------------------------------------------------------------

class Status final {
  public:
    explicit Status(long mt_type, long mt_resid, long mt_dsreg, long mt_gstat,
                    long mt_erreg, int mt_fileno, int mt_blkno);

    [[nodiscard]] [[nodiscard]] long type() const noexcept { return type_; }
    [[nodiscard]] [[nodiscard]] long resid() const noexcept { return resid_; }
    [[nodiscard]] [[nodiscard]] long dsreg() const noexcept { return dsreg_; }
    [[nodiscard]] [[nodiscard]] long gstat() const noexcept { return gstat_; }
    [[nodiscard]] [[nodiscard]] long erreg() const noexcept { return erreg_; }
    [[nodiscard]] [[nodiscard]] int fileno() const noexcept { return fileno_; }
    [[nodiscard]] [[nodiscard]] int blkno() const noexcept { return blkno_; }

    [[nodiscard]] [[nodiscard]] bool eof() const noexcept;
    [[nodiscard]] [[nodiscard]] bool bot() const noexcept;
    [[nodiscard]] [[nodiscard]] bool eot() const noexcept;
    [[nodiscard]] [[nodiscard]] bool sm() const noexcept;
    [[nodiscard]] [[nodiscard]] bool eod() const noexcept;
    [[nodiscard]] [[nodiscard]] bool wr_prot() const noexcept;
    [[nodiscard]] [[nodiscard]] bool online() const noexcept;
    [[nodiscard]] [[nodiscard]] bool dr_open() const noexcept;
    [[nodiscard]] [[nodiscard]] bool cleaning_requested() const noexcept;

    [[nodiscard]] [[nodiscard]] int density_code() const noexcept;
    [[nodiscard]] [[nodiscard]] int block_size() const noexcept;
    [[nodiscard]] [[nodiscard]] std::string_view density_name() const;

    [[nodiscard]] [[nodiscard]] std::string type_name() const;

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

enum class TapeBlockMode {
    variable,
    fixed,
};

struct TapeBlockModeResult {
    TapeBlockMode mode;
    uint32_t block_size;
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

    TapeDevice(TapeDevice &&other) noexcept;
    TapeDevice &operator=(TapeDevice &&other) noexcept;

    // -- accessors -----------------------------------------------------

    // fd() is virtual — see declaration near bottom of class
    [[nodiscard]] [[nodiscard]] const std::string &
    device_path() const noexcept {
        return device_path_;
    }
    [[nodiscard]] [[nodiscard]] bool is_read_write() const noexcept {
        return read_write_;
    }
    void close();
    void reopen();

    // -- I/O -----------------------------------------------------------

    // Write all bytes to the tape device. Throws on short write or error.
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
    void space_fwd_setmarks(int count = 1);
    void space_bwd_setmarks(int count = 1);

    void seek_block(long block_no);
    Position tell();

    // -- markers -------------------------------------------------------

    void write_filemark(int count = 1);
    void write_setmark(int count = 1);
    void erase(int count = 1);

    // -- drive control -------------------------------------------------

    void set_block_size(int bytes);
    TapeBlockModeResult
    configure_preferred_variable_block_mode(uint32_t fallback_block_size,
                                            std::string_view context,
                                            std::ostream &warnings);
    void set_density(int code);
    void set_compression(bool enable);
    void lock();
    void unlock();
    void load(int count = 1);
    void offline();

    // -- status queries ------------------------------------------------

    Status status();
    bool is_online();
    bool is_write_protected();
    int get_fileno();
    int get_blkno();

    // -- static helpers ------------------------------------------------

    static std::string_view density_name(int code);
    static bool is_scsi_tape(int fd);

    // fd() is virtual so test doubles can return a different fd
    [[nodiscard]] [[nodiscard]] virtual int fd() const noexcept { return fd_; }

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

    [[nodiscard]] [[nodiscard]] int fd() const noexcept override;

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
    std::size_t read_index_ = 0;
    uint64_t next_file_num_ = 0;
    uint64_t current_file_num_ = 0;
    uint64_t current_record_ = 0;
    uint32_t current_block_size_ = 0;
    bool exhausted_ = false;
    bool current_is_temp_ = false;
    std::filesystem::path current_path_;

    void finalize_current_file();
};

} // namespace mt
