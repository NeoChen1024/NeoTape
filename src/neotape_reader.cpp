#include "neotape/reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace neotape {

namespace fs = std::filesystem;
using std::size_t;

SpoolVolumeReader::SpoolVolumeReader(const fs::path &volume_dir)
    : volume_dir_(volume_dir) {
    scan_files();
    if (tape_files_.empty())
        throw std::runtime_error(
            std::format("no tape files in volume {}", volume_dir.string()));
    read_volume_header();
}

SpoolVolumeReader::~SpoolVolumeReader() { close_file(); }

void SpoolVolumeReader::scan_files() {
    tape_files_.clear();
    file_idx_ = 0;

    static constexpr std::string_view prefix = "tape-file-";
    static constexpr std::string_view ext = ".nts";

    std::vector<std::pair<uint64_t, fs::path>> indexed;

    for (const auto &entry : fs::directory_iterator(volume_dir_)) {
        if (!entry.is_regular_file())
            continue;
        auto name = entry.path().filename().string();
        if (name.size() <= prefix.size() + ext.size() ||
            name.substr(0, prefix.size()) != prefix ||
            name.substr(name.size() - ext.size()) != ext)
            continue;
        auto dot = name.find('.', prefix.size());
        if (dot == std::string::npos)
            continue;
        auto num_str = name.substr(prefix.size(), dot - prefix.size());
        if (num_str.empty())
            continue;
        char *end = nullptr;
        uint64_t num = std::strtoull(num_str.c_str(), &end, 10);
        if (end == nullptr || *end != '\0')
            continue;
        indexed.emplace_back(num, entry.path());
    }

    std::ranges::sort(indexed, {}, &std::pair<uint64_t, fs::path>::first);

    tape_files_.reserve(indexed.size());
    for (auto &p : indexed)
        tape_files_.push_back(std::move(p.second));
}

void SpoolVolumeReader::read_volume_header() {
    close_file();

    for (std::size_t i = 0; i < tape_files_.size(); ++i) {
        std::FILE *fh = std::fopen(tape_files_[i].c_str(), "rb");
        if (fh == nullptr)
            throw std::runtime_error(std::format(
                "open {}: {}", tape_files_[i].string(), std::strerror(errno)));

        std::vector<uint8_t> buf(fixed_header_size);
        if (std::fread(buf.data(), 1, fixed_header_size, fh) !=
            fixed_header_size) {
            std::fclose(fh);
            throw std::runtime_error(
                std::format("short read from {}: expected {} bytes",
                            tape_files_[i].string(), fixed_header_size));
        }

        auto parsed = parse_fixed_header(buf.data(), buf.size());

        if (parsed.type == HeaderType::medium) {
            std::fclose(fh);
            continue;
        }

        if (parsed.type != HeaderType::volume) {
            std::fclose(fh);
            throw std::runtime_error(std::format(
                "expected volume header in {}, got {}", tape_files_[i].string(),
                header_type_name(parsed.type)));
        }

        // Found volume header — read entire file for validation
        std::fclose(fh);
        fh = std::fopen(tape_files_[i].c_str(), "rb");
        if (fh == nullptr)
            throw std::runtime_error(std::format(
                "open {}: {}", tape_files_[i].string(), std::strerror(errno)));

        std::error_code ec;
        uintmax_t file_size = fs::file_size(tape_files_[i], ec);
        if (ec) {
            std::fclose(fh);
            throw std::runtime_error(std::format(
                "stat {}: {}", tape_files_[i].string(), ec.message()));
        }
        if (file_size < fixed_header_size) {
            std::fclose(fh);
            throw std::runtime_error("volume header file too short");
        }

        buf.resize(static_cast<size_t>(file_size));
        if (std::fread(buf.data(), 1, buf.size(), fh) != buf.size()) {
            std::fclose(fh);
            throw std::runtime_error("read volume header failed");
        }
        std::fclose(fh);

        parsed = parse_fixed_header(buf.data(), buf.size());

        volume_header_ = *parsed.volume;
        block_size_ = volume_header_.volume_block_size;
        if (!valid_block_size(block_size_))
            throw std::runtime_error("invalid volume block size");
        if (static_cast<uint64_t>(file_size) != block_size_)
            throw std::runtime_error(
                "volume header file size does not match block size");

        file_idx_ = i + 1;
        return;
    }

    throw std::runtime_error(
        std::format("no volume header found in {}", volume_dir_.string()));
}

void SpoolVolumeReader::close_file() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool SpoolVolumeReader::open_file(std::size_t index) {
    close_file();
    if (index >= tape_files_.size())
        return false;
    file_ = std::fopen(tape_files_[index].c_str(), "rb");
    if (file_ == nullptr)
        throw std::runtime_error(std::format(
            "open {}: {}", tape_files_[index].string(), std::strerror(errno)));
    return true;
}

bool SpoolVolumeReader::read_record(std::vector<uint8_t> &out) {
    if (exhausted_ || file_ == nullptr)
        return false;

    out.resize(block_size_);
    size_t n = std::fread(out.data(), 1, block_size_, file_);
    if (n == 0) {
        out.clear();
        return false;
    }
    if (n != block_size_)
        throw std::runtime_error(
            std::format("short read at tape file {}: expected {} bytes, got {}",
                        tape_file_num_, block_size_, n));
    return true;
}

bool SpoolVolumeReader::next_file() {
    close_file();
    if (file_idx_ >= tape_files_.size()) {
        exhausted_ = true;
        return false;
    }
    ++tape_file_num_;
    return open_file(file_idx_++);
}

TapeDeviceVolumeReader::TapeDeviceVolumeReader(mt::TapeDevice &device)
    : device_(device) {
    std::vector<uint8_t> buf(8 * 1024 * 1024);
    ssize_t n = ::read(device_.fd(), buf.data(), buf.size());
    if (n < 0)
        throw std::runtime_error(
            std::format("read volume header: {}", std::strerror(errno)));
    if (static_cast<std::size_t>(n) < fixed_header_size)
        throw std::runtime_error("short read from tape volume header");

    buf.resize(static_cast<std::size_t>(n));
    auto parsed = parse_fixed_header(buf.data(), buf.size());
    if (parsed.medium) {
        device_.space_fwd();
        buf.assign(8 * 1024 * 1024, 0);
        n = ::read(device_.fd(), buf.data(), buf.size());
        if (n < 0)
            throw std::runtime_error(std::format(
                "read volume header after medium header: {}",
                std::strerror(errno)));
        if (static_cast<std::size_t>(n) < fixed_header_size)
            throw std::runtime_error(
                "short read from tape volume header after medium header");
        buf.resize(static_cast<std::size_t>(n));
        parsed = parse_fixed_header(buf.data(), buf.size());
        if (!parsed.volume)
            throw std::runtime_error(std::format(
                "expected volume header after medium header, got {}",
                header_type_name(parsed.type)));
    }
    if (!parsed.volume)
        throw std::runtime_error(std::format("expected volume header, got {}",
                                             header_type_name(parsed.type)));

    volume_header_ = *parsed.volume;
    block_size_ = volume_header_.volume_block_size;
    if (!valid_block_size(block_size_))
        throw std::runtime_error("invalid volume block size");
}

bool TapeDeviceVolumeReader::next_file() {
    if (exhausted_)
        return false;
    try {
        device_.space_fwd();
    } catch (const mt::Error &) {
        exhausted_ = true;
        return false;
    }
    if (device_.fd() < 0) {
        exhausted_ = true;
        return false;
    }
    ++tape_file_num_;
    return true;
}

bool TapeDeviceVolumeReader::read_record(std::vector<uint8_t> &out) {
    if (exhausted_ || device_.fd() < 0)
        return false;
    out.resize(block_size_);
    ssize_t n = ::read(device_.fd(), out.data(), out.size());
    if (n == 0) {
        out.clear();
        return false;
    }
    if (n < 0)
        throw std::runtime_error(
            std::format("read tape record: {}", std::strerror(errno)));
    if (static_cast<std::size_t>(n) != block_size_)
        throw std::runtime_error(
            std::format("short read at tape file {}: expected {} bytes, got {}",
                        tape_file_num_, block_size_, n));
    return true;
}

} // namespace neotape
