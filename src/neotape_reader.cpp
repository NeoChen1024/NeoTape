#include "neotape/reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
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
	file_idx_ = 1;
}

SpoolVolumeReader::~SpoolVolumeReader() { close_file(); }

void SpoolVolumeReader::scan_files() {
	tape_files_.clear();
	file_idx_ = 0;
	for (const auto &entry : fs::directory_iterator(volume_dir_)) {
		if (entry.is_regular_file())
			tape_files_.push_back(entry.path());
	}
	std::ranges::sort(tape_files_);
}

void SpoolVolumeReader::read_volume_header() {
	close_file();

	file_ = std::fopen(tape_files_[0].c_str(), "rb");
	if (file_ == nullptr)
		throw std::runtime_error(std::format("open {}: {}",
		    tape_files_[0].string(), std::strerror(errno)));

	std::error_code ec;
	uintmax_t file_size = fs::file_size(tape_files_[0], ec);
	if (ec)
		throw std::runtime_error(
		    std::format("stat {}: {}", tape_files_[0].string(), ec.message()));
	if (file_size < fixed_header_size)
		throw std::runtime_error("volume header file too short");

	std::vector<uint8_t> buf(static_cast<size_t>(file_size));
	if (std::fread(buf.data(), 1, buf.size(), file_) != buf.size())
		throw std::runtime_error("read volume header failed");
	close_file();

	auto parsed = parse_fixed_header(buf.data(), buf.size());
	if (!parsed.volume)
		throw std::runtime_error("first tape file is not a volume header");

	volume_header_ = *parsed.volume;
	block_size_ = volume_header_.volume_block_size;
	if (!valid_block_size(block_size_))
		throw std::runtime_error("invalid volume block size");
	if (static_cast<uint64_t>(file_size) != block_size_)
		throw std::runtime_error("volume header file size does not match block size");
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
		throw std::runtime_error(std::format("open {}: {}",
		    tape_files_[index].string(), std::strerror(errno)));
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

} // namespace neotape
