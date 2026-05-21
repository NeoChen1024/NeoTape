#pragma once

#include "neotape/format.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace neotape {

class VirtualTapeReader {
  public:
    virtual bool read_record(std::vector<uint8_t> &out) = 0;
    virtual bool next_file() = 0;
    virtual uint32_t block_size() const = 0;
    virtual uint64_t tape_file_num() const = 0;
    virtual bool exhausted() const = 0;
    virtual ~VirtualTapeReader() = default;
};

class SpoolVolumeReader final : public VirtualTapeReader {
  public:
    explicit SpoolVolumeReader(const std::filesystem::path &volume_dir);
    ~SpoolVolumeReader() override;

    SpoolVolumeReader(const SpoolVolumeReader &) = delete;
    SpoolVolumeReader &operator=(const SpoolVolumeReader &) = delete;
    SpoolVolumeReader(SpoolVolumeReader &&) = delete;
    SpoolVolumeReader &operator=(SpoolVolumeReader &&) = delete;

    bool read_record(std::vector<uint8_t> &out) override;
    bool next_file() override;
    uint32_t block_size() const override { return block_size_; }
    uint64_t tape_file_num() const override { return tape_file_num_; }
    bool exhausted() const override { return exhausted_; }

    const VolumeHeader &volume_header() const { return volume_header_; }
    uint64_t volume_seq_num() const { return volume_header_.volume_seq_num; }

  private:
    void scan_files();
    void read_volume_header();
    bool open_file(std::size_t index);
    void close_file();

    std::filesystem::path volume_dir_;
    std::vector<std::filesystem::path> tape_files_;
    std::size_t file_idx_ = 0;

    std::FILE *file_ = nullptr;
    VolumeHeader volume_header_;
    uint32_t block_size_ = 0;
    uint64_t tape_file_num_ = 0;
    bool exhausted_ = false;
};

} // namespace neotape
