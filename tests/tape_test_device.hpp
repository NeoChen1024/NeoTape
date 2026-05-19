#pragma once

#include "neotape/tape.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mt {
namespace test {

class FileBackedTapeDevice final : public TapeDevice {
public:
    explicit FileBackedTapeDevice(std::string_view path,
                                  uint32_t block_size = 65536,
                                  bool read_write = true);
    ~FileBackedTapeDevice() override;

    void   do_mtop(int op, int count) override;
    Status do_status() override;
    Position do_tell() override;    // throws — LTO-style

    int fd() const noexcept override { return backing_fd_; }

    size_t file_count() const noexcept { return boundaries_.size(); }
    bool   at_eod()  const noexcept { return !boundaries_.empty() && static_cast<std::size_t>(current_file_) >= boundaries_.size(); }
    uint32_t block_size() const noexcept { return block_size_; }

private:
    void seek_to_current_file();

    int backing_fd_;
    std::vector<uint64_t> boundaries_;
    int      current_file_ = -1;
    uint64_t current_record_ = 0;
    uint32_t block_size_;
    bool     read_write_;
};

} // namespace test
} // namespace mt
