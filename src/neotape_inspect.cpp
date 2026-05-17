#include "neotape/format.hpp"

#include <blake3.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Options {
	std::filesystem::path spool_dir;
};

struct InspectState {
	std::string archive_uuid;
	uint32_t volume_block_size = 0;
	uint64_t expected_volume_seq_num = 1;
	uint64_t expected_slice_seq_num = 1;
	uint64_t expected_global_frame_seq_num = 1;
	bool saw_archive_end = false;
	blake3_hasher slice_hasher;
	uint64_t slice_size = 0;
	bool slice_open = false;
};

[[noreturn]] void fail(const std::string &message) {
	std::cerr << std::format("neotape-inspect: {}\n", message);
	std::exit(1);
}

void usage(const char *prog) {
	std::cerr << std::format("usage: {} <spool-dir>\n", prog);
}

Options parse_args(int argc, char **argv) {
	if (argc != 2) {
		usage(argv[0]);
		std::exit(2);
	}
	return Options{.spool_dir = argv[1]};
}

std::vector<std::filesystem::path> sorted_dirs(const std::filesystem::path &root) {
	std::vector<std::filesystem::path> dirs;
	for (const auto &entry : std::filesystem::directory_iterator(root)) {
		if (entry.is_directory())
			dirs.push_back(entry.path());
	}
	std::ranges::sort(dirs);
	return dirs;
}

std::vector<std::filesystem::path> sorted_files(const std::filesystem::path &root) {
	std::vector<std::filesystem::path> files;
	for (const auto &entry : std::filesystem::directory_iterator(root)) {
		if (entry.is_regular_file())
			files.push_back(entry.path());
	}
	std::ranges::sort(files);
	return files;
}

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		fail(std::format("open {}", path.string()));
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
	    std::istreambuf_iterator<char>());
}

void require(bool condition, const std::string &message) {
	if (!condition)
		fail(message);
}

void inspect_volume(const std::filesystem::path &path, InspectState &state,
    const neotape::VolumeHeader &header, std::size_t file_size) {
	require(header.volume_seq_num == state.expected_volume_seq_num,
	    std::format("{}: expected volume {}, got {}", path.string(),
		state.expected_volume_seq_num, header.volume_seq_num));
	require(neotape::valid_block_size(header.volume_block_size),
	    std::format("{}: invalid volume block size", path.string()));
	require(file_size == header.volume_block_size,
	    std::format("{}: volume header file size does not match block size",
		path.string()));
	if (state.archive_uuid.empty()) {
		state.archive_uuid = header.archive_uuid;
		state.volume_block_size = header.volume_block_size;
	} else {
		require(header.archive_uuid == state.archive_uuid,
		    std::format("{}: archive uuid mismatch", path.string()));
		require(header.volume_block_size == state.volume_block_size,
		    std::format("{}: volume block size changed", path.string()));
	}
	std::cout << std::format("{}: volume seq={} block={} archive={}\n",
	    path.string(), header.volume_seq_num, header.volume_block_size,
	    header.archive_uuid);
	++state.expected_volume_seq_num;
}

void verify_zero_padding(const std::filesystem::path &path,
    const std::vector<uint8_t> &bytes, std::size_t begin) {
	for (std::size_t i = begin; i < bytes.size(); ++i) {
		if (bytes[i] != 0)
			fail(std::format("{}: non-zero padding at byte {}", path.string(), i));
	}
}

void inspect_frame(const std::filesystem::path &path, InspectState &state,
    const neotape::FrameHeader &header, const std::vector<uint8_t> &bytes) {
	require(header.archive_uuid == state.archive_uuid,
	    std::format("{}: archive uuid mismatch", path.string()));
	require(header.volume_block_size == state.volume_block_size,
	    std::format("{}: block size mismatch", path.string()));
	require(bytes.size() == header.volume_block_size,
	    std::format("{}: frame record size does not match block size", path.string()));
	require(header.frame_payload_size <= header.volume_block_size - neotape::fixed_header_size,
	    std::format("{}: frame payload too large", path.string()));
	require(header.global_frame_seq_num == state.expected_global_frame_seq_num,
	    std::format("{}: expected global frame {}, got {}", path.string(),
		state.expected_global_frame_seq_num, header.global_frame_seq_num));

	std::size_t payload_begin = neotape::fixed_header_size;
	std::size_t payload_end =
	    payload_begin + static_cast<std::size_t>(header.frame_payload_size);
	neotape::Hash payload_hash =
	    neotape::blake3_hash(bytes.data() + payload_begin, header.frame_payload_size);
	require(payload_hash == header.frame_payload_blake3,
	    std::format("{}: frame payload BLAKE3 mismatch", path.string()));
	verify_zero_padding(path, bytes, payload_end);

	bool start = (header.flags & neotape::frame_flag_start) != 0;
	bool end = (header.flags & neotape::frame_flag_end) != 0;
	if (header.frame_content_type == neotape::FrameContentType::slice_content) {
		if (start) {
			require(!state.slice_open,
			    std::format("{}: new slice starts before previous slice ended",
				path.string()));
			require(header.logical_slice_seq_num == state.expected_slice_seq_num,
			    std::format("{}: expected slice {}, got {}", path.string(),
				state.expected_slice_seq_num, header.logical_slice_seq_num));
			blake3_hasher_init(&state.slice_hasher);
			state.slice_size = 0;
			state.slice_open = true;
		}
		require(state.slice_open,
		    std::format("{}: content frame without open slice", path.string()));
		blake3_hasher_update(&state.slice_hasher, bytes.data() + payload_begin,
		    header.frame_payload_size);
		state.slice_size += header.frame_payload_size;
		if (end) {
			neotape::Hash slice_hash{};
			blake3_hasher_finalize(&state.slice_hasher, slice_hash.data(),
			    slice_hash.size());
			require(header.slice_content_size == state.slice_size,
			    std::format("{}: slice size mismatch", path.string()));
			require(slice_hash == header.slice_content_blake3,
			    std::format("{}: slice BLAKE3 mismatch", path.string()));
			state.slice_open = false;
			++state.expected_slice_seq_num;
		}
	}

	std::cout << std::format(
	    "{}: frame global={} slice={} within={} payload={} type={} flags=0x{:04x}\n",
	    path.string(), header.global_frame_seq_num, header.logical_slice_seq_num,
	    header.frame_seq_num_within_slice, header.frame_payload_size,
	    neotape::frame_content_type_name(header.frame_content_type), header.flags);
	++state.expected_global_frame_seq_num;
}

void inspect_archive_end(const std::filesystem::path &path, InspectState &state,
    const neotape::ArchiveEndHeader &header, std::size_t file_size) {
	require(header.archive_uuid == state.archive_uuid,
	    std::format("{}: archive uuid mismatch", path.string()));
	require(header.volume_block_size == state.volume_block_size,
	    std::format("{}: block size mismatch", path.string()));
	require(file_size == header.volume_block_size,
	    std::format("{}: archive end file size does not match block size",
		path.string()));
	require(!state.slice_open, std::format("{}: archive ended with open slice", path.string()));
	require((header.flags & neotape::archive_end_flag_clean_end) != 0,
	    std::format("{}: CLEAN_END flag is not set", path.string()));
	require(header.last_logical_slice_seq_num + 1 == state.expected_slice_seq_num,
	    std::format("{}: last slice sequence mismatch", path.string()));
	require(header.last_global_frame_seq_num + 1 == state.expected_global_frame_seq_num,
	    std::format("{}: last frame sequence mismatch", path.string()));
	std::cout << std::format("{}: archive_end last_slice={} last_frame={}\n",
	    path.string(), header.last_logical_slice_seq_num,
	    header.last_global_frame_seq_num);
	state.saw_archive_end = true;
}

void inspect_file(const std::filesystem::path &path, InspectState &state) {
	std::vector<uint8_t> bytes = read_file(path);
	require(bytes.size() >= neotape::fixed_header_size,
	    std::format("{}: shorter than fixed header", path.string()));
	neotape::ParsedHeader parsed =
	    neotape::parse_fixed_header(bytes.data(), bytes.size());
	if (parsed.volume)
		inspect_volume(path, state, *parsed.volume, bytes.size());
	else if (parsed.frame) {
		require(state.volume_block_size != 0,
		    std::format("{}: frame appears before volume header", path.string()));
		require(bytes.size() % state.volume_block_size == 0,
		    std::format("{}: slice file size is not a multiple of block size",
			path.string()));
		for (std::size_t offset = 0; offset < bytes.size();
		     offset += state.volume_block_size) {
			std::vector<uint8_t> record(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
			    bytes.begin() + static_cast<std::ptrdiff_t>(offset + state.volume_block_size));
			neotape::ParsedHeader frame =
			    neotape::parse_fixed_header(record.data(), record.size());
			require(frame.frame.has_value(),
			    std::format("{}: non-frame record inside slice file", path.string()));
			inspect_frame(path, state, *frame.frame, record);
		}
	} else if (parsed.archive_end)
		inspect_archive_end(path, state, *parsed.archive_end, bytes.size());
}

void inspect_spool(const Options &opts) {
	require(std::filesystem::is_directory(opts.spool_dir),
	    std::format("{} is not a directory", opts.spool_dir.string()));

	InspectState state;
	for (const std::filesystem::path &volume_dir : sorted_dirs(opts.spool_dir)) {
		for (const std::filesystem::path &file : sorted_files(volume_dir)) {
			inspect_file(file, state);
			if (state.saw_archive_end)
				break;
		}
		if (state.saw_archive_end)
			break;
	}
	require(state.saw_archive_end, "missing Archive End Header");
	std::cout << std::format("ok: archive {} volumes={} slices={} frames={}\n",
	    state.archive_uuid, state.expected_volume_seq_num - 1,
	    state.expected_slice_seq_num - 1, state.expected_global_frame_seq_num - 1);
}

} // namespace

int main(int argc, char **argv) {
	try {
		Options opts = parse_args(argc, argv);
		inspect_spool(opts);
		return 0;
	} catch (const std::exception &e) {
		fail(e.what());
	}
}
