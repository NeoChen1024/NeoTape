#include "tape_test_device.hpp"
#include "neotape/tape_ioctl.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace mt {
namespace test {

FileBackedTapeDevice::FileBackedTapeDevice(std::string_view path,
                                           uint32_t block_size,
                                           bool read_write)
    : TapeDevice(-1, std::string(path), read_write),
      block_size_(block_size),
      read_write_(read_write)
{
    int oflags = read_write ? O_RDWR | O_CREAT : O_RDONLY;
    backing_fd_ = ::open(std::string(path).c_str(), oflags, 0666);
    if (backing_fd_ < 0)
        throw Error(std::string(path), "open", errno);
}

FileBackedTapeDevice::~FileBackedTapeDevice() {
    if (backing_fd_ >= 0)
        ::close(backing_fd_);
}

// Model: boundaries_ stores file END offsets (byte offset right after
// each filemark).  file_count() == boundaries_.size().
// current_file_:
//   -1 = BOT                     → seek to 0
//    0 .. size-1 = at file i     → seek to start of file i
//    size = EOD (past last file) → seek to end of data

void FileBackedTapeDevice::seek_to_current_file() {
    off_t off = 0;
    if (current_file_ >= 0) {
        if (static_cast<std::size_t>(current_file_) < boundaries_.size())
            off = (current_file_ == 0) ? 0
                 : static_cast<off_t>(boundaries_[current_file_ - 1]);
        else
            off = boundaries_.empty() ? 0
                 : static_cast<off_t>(boundaries_.back());
    }
    ::lseek(backing_fd_, off, SEEK_SET);
}

void FileBackedTapeDevice::do_mtop(int op, int count) {
    switch (op) {
    case MTREW:
        current_file_ = -1;
        current_record_ = 0;
        ::lseek(backing_fd_, 0, SEEK_SET);
        break;

    case MTWEOF: {
        off_t pos = ::lseek(backing_fd_, 0, SEEK_CUR);
        if (pos < 0) throw Error(device_path(), "lseek", errno);
        for (int i = 0; i < count; i++)
            boundaries_.push_back(static_cast<uint64_t>(pos));
        current_file_ = static_cast<int>(boundaries_.size());
        current_record_ = 0;
        break;
    }

    case MTEOM:
        if (boundaries_.empty()) {
            current_file_ = -1;
            ::lseek(backing_fd_, 0, SEEK_SET);
        } else {
            current_file_ = static_cast<int>(boundaries_.size());
            ::lseek(backing_fd_, static_cast<off_t>(boundaries_.back()), SEEK_SET);
        }
        current_record_ = 0;
        break;

    case MTFSF:
        current_file_ = std::min(
            current_file_ + count,
            static_cast<int>(boundaries_.size()));
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTBSF:
        current_file_ = std::max(current_file_ - count, -1);
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTFSFM:
        current_file_ = std::min(
            current_file_ + count,
            static_cast<int>(boundaries_.size()));
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTBSFM:
        current_file_ = std::max(current_file_ - count, -1);
        current_record_ = 0;
        seek_to_current_file();
        break;

    case MTFSR:
        current_record_ += static_cast<uint64_t>(count);
        seek_to_current_file();
        break;

    case MTBSR:
        if (static_cast<int64_t>(current_record_) >= count)
            current_record_ -= static_cast<uint64_t>(count);
        else
            current_record_ = 0;
        seek_to_current_file();
        break;

    case MTSEEK:
    case MTTELL:
        throw Error(device_path(), "mtop", ENOTSUP);

    case MTERASE:
        break;

    default:
        throw Error(device_path(), "mtop", ENOTSUP);
    }
}

Status FileBackedTapeDevice::do_status() {
    bool bot = (current_file_ < 0);
    return Status(
        0,
        0,
        (static_cast<long>(block_size_) & MT_ST_BLKSIZE_MASK),
        (bot ? GMT_BOT : 0) | GMT_ONLINE | GMT_EOD,
        0,
        bot ? 0 : current_file_,
        static_cast<int>(current_record_)
    );
}

Position FileBackedTapeDevice::do_tell() {
    throw Error(device_path(), "tell", ENOTSUP);
}

} // namespace test
} // namespace mt
