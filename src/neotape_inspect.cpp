#include "neotape/format.hpp"

#include <blake3.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <string>
#include <vector>

namespace {

// ====================== Inspector State ==========================

namespace fs = std::filesystem;
using std::format;
using std::size_t;
using std::string;
using std::vector;

struct Options {
	fs::path spool_dir;
};

struct InspectState {
	string archive_uuid;
	uint32_t volume_block_size = 0;
	uint64_t expected_volume_seq_num = 1;
	uint64_t expected_slice_seq_num = 1;
	uint64_t expected_global_frame_seq_num = 1;
	bool saw_archive_end = false;
	blake3_hasher slice_hasher;
	uint64_t slice_size = 0;
	bool slice_open = false;
};

// ====================== Diagnostics & File IO ====================

[[noreturn]] void fail(const string &message) {
	std::cerr << format("neotape-inspect: {}\n", message);
	std::exit(1);
}

void usage(const char *prog) {
	std::cerr << format("usage: {} <spool-dir>\n", prog);
}

Options parse_args(int argc, char **argv) {
	static const struct option long_opts[] = {
		{"help", no_argument, nullptr, 'h'},
		{nullptr, 0, nullptr, 0}
	};
	int c;
	while ((c = getopt_long(argc, argv, "h", long_opts, nullptr)) != -1) {
		switch (c) {
		case 'h': usage(argv[0]); std::exit(0);
		case '?': std::exit(2);
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		std::exit(2);
	}
	return Options{.spool_dir = argv[optind]};
}

vector<fs::path> sorted_dirs(const fs::path &root) {
	vector<fs::path> dirs;
	for (const auto &entry : fs::directory_iterator(root)) {
		if (entry.is_directory())
			dirs.push_back(entry.path());
	}
	std::ranges::sort(dirs);
	return dirs;
}

vector<fs::path> sorted_files(const fs::path &root) {
	vector<fs::path> files;
	for (const auto &entry : fs::directory_iterator(root)) {
		if (entry.is_regular_file())
			files.push_back(entry.path());
	}
	std::ranges::sort(files);
	return files;
}

vector<uint8_t> read_file(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		fail(format("open {}", path.string()));
	return vector<uint8_t>((std::istreambuf_iterator<char>(in)),
	    std::istreambuf_iterator<char>());
}

void require(bool condition, const string &message) {
	if (!condition)
		fail(message);
}

// ====================== Record Validation ========================

void inspect_volume(const fs::path &path, InspectState &state,
    const neotape::VolumeHeader &header, size_t file_size) {
	require(header.volume_seq_num == state.expected_volume_seq_num,
	    format("{}: expected volume {}, got {}", path.string(),
		state.expected_volume_seq_num, header.volume_seq_num));
	require(neotape::valid_block_size(header.volume_block_size),
	    format("{}: invalid volume block size", path.string()));
	require(file_size == header.volume_block_size,
	    format("{}: volume header file size does not match block size",
		path.string()));
	if (state.archive_uuid.empty()) {
		state.archive_uuid = header.archive_uuid;
		state.volume_block_size = header.volume_block_size;
	} else {
		require(header.archive_uuid == state.archive_uuid,
		    format("{}: archive uuid mismatch", path.string()));
		require(header.volume_block_size == state.volume_block_size,
		    format("{}: volume block size changed", path.string()));
	}
	std::cout << format("{}: volume seq={} block={} archive={}\n",
	    path.string(), header.volume_seq_num, header.volume_block_size,
	    header.archive_uuid);
	++state.expected_volume_seq_num;
}

void verify_zero_padding(const fs::path &path, const vector<uint8_t> &bytes,
    size_t begin) {
	for (size_t i = begin; i < bytes.size(); ++i) {
		if (bytes[i] != 0)
			fail(format("{}: non-zero padding at byte {}", path.string(), i));
	}
}

void inspect_frame(const fs::path &path, InspectState &state,
    const neotape::FrameHeader &header, const vector<uint8_t> &bytes) {
	require(header.archive_uuid == state.archive_uuid,
	    format("{}: archive uuid mismatch", path.string()));
	require(header.volume_block_size == state.volume_block_size,
	    format("{}: block size mismatch", path.string()));
	require(bytes.size() == header.volume_block_size,
	    format("{}: frame record size does not match block size", path.string()));
	require(header.frame_payload_size <= header.volume_block_size - neotape::fixed_header_size,
	    format("{}: frame payload too large", path.string()));
	require(header.global_frame_seq_num == state.expected_global_frame_seq_num,
	    format("{}: expected global frame {}, got {}", path.string(),
		state.expected_global_frame_seq_num, header.global_frame_seq_num));

	size_t payload_begin = neotape::fixed_header_size;
	size_t payload_end = payload_begin + static_cast<size_t>(header.frame_payload_size);
	neotape::Hash payload_hash =
	    neotape::blake3_hash(bytes.data() + payload_begin, header.frame_payload_size);
	require(payload_hash == header.frame_payload_blake3,
	    format("{}: frame payload BLAKE3 mismatch", path.string()));
	verify_zero_padding(path, bytes, payload_end);

	bool start = (header.flags & neotape::frame_flag_start) != 0;
	bool end = (header.flags & neotape::frame_flag_end) != 0;
	if (header.frame_content_type == neotape::FrameContentType::slice_content) {
		// Slice digests are accumulated across content frames and verified only
		// when the END flag appears.
		if (start) {
			require(!state.slice_open,
			    format("{}: new slice starts before previous slice ended",
				path.string()));
			require(header.logical_slice_seq_num == state.expected_slice_seq_num,
			    format("{}: expected slice {}, got {}", path.string(),
				state.expected_slice_seq_num, header.logical_slice_seq_num));
			blake3_hasher_init(&state.slice_hasher);
			state.slice_size = 0;
			state.slice_open = true;
		}
		require(state.slice_open,
		    format("{}: content frame without open slice", path.string()));
		blake3_hasher_update(&state.slice_hasher, bytes.data() + payload_begin,
		    header.frame_payload_size);
		state.slice_size += header.frame_payload_size;
		if (end) {
			neotape::Hash slice_hash{};
			blake3_hasher_finalize(&state.slice_hasher, slice_hash.data(),
			    slice_hash.size());
			require(header.slice_content_size == state.slice_size,
			    format("{}: slice size mismatch", path.string()));
			require(slice_hash == header.slice_content_blake3,
			    format("{}: slice BLAKE3 mismatch", path.string()));
			state.slice_open = false;
			++state.expected_slice_seq_num;
		}
	}

	std::cout << format(
	    "{}: frame global={} slice={} within={} payload={} type={} flags=0x{:04x}\n",
	    path.string(), header.global_frame_seq_num, header.logical_slice_seq_num,
	    header.frame_seq_num_within_slice, header.frame_payload_size,
	    neotape::frame_content_type_name(header.frame_content_type), header.flags);
	++state.expected_global_frame_seq_num;
}

void inspect_archive_end(const fs::path &path, InspectState &state,
    const neotape::ArchiveEndHeader &header, size_t file_size) {
	require(header.archive_uuid == state.archive_uuid,
	    format("{}: archive uuid mismatch", path.string()));
	require(header.volume_block_size == state.volume_block_size,
	    format("{}: block size mismatch", path.string()));
	require(file_size == header.volume_block_size,
	    format("{}: archive end file size does not match block size",
		path.string()));
	require(!state.slice_open, format("{}: archive ended with open slice", path.string()));
	require((header.flags & neotape::archive_end_flag_clean_end) != 0,
	    format("{}: CLEAN_END flag is not set", path.string()));
	require(header.last_logical_slice_seq_num + 1 == state.expected_slice_seq_num,
	    format("{}: last slice sequence mismatch", path.string()));
	require(header.last_global_frame_seq_num + 1 == state.expected_global_frame_seq_num,
	    format("{}: last frame sequence mismatch", path.string()));
	std::cout << format("{}: archive_end last_slice={} last_frame={}\n",
	    path.string(), header.last_logical_slice_seq_num,
	    header.last_global_frame_seq_num);
	state.saw_archive_end = true;
}

// ====================== Spool Traversal ==========================

void inspect_file(const fs::path &path, InspectState &state) {
	vector<uint8_t> bytes = read_file(path);
	require(bytes.size() >= neotape::fixed_header_size,
	    format("{}: shorter than fixed header", path.string()));
	neotape::ParsedHeader parsed =
	    neotape::parse_fixed_header(bytes.data(), bytes.size());
	if (parsed.volume)
		inspect_volume(path, state, *parsed.volume, bytes.size());
	else if (parsed.frame) {
		require(state.volume_block_size != 0,
		    format("{}: frame appears before volume header", path.string()));
		require(bytes.size() % state.volume_block_size == 0,
		    format("{}: slice file size is not a multiple of block size",
			path.string()));
		for (size_t offset = 0; offset < bytes.size();
		     offset += state.volume_block_size) {
			vector<uint8_t> record(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
			    bytes.begin() + static_cast<std::ptrdiff_t>(offset + state.volume_block_size));
			neotape::ParsedHeader frame =
			    neotape::parse_fixed_header(record.data(), record.size());
			require(frame.frame.has_value(),
			    format("{}: non-frame record inside slice file", path.string()));
			inspect_frame(path, state, *frame.frame, record);
		}
	} else if (parsed.archive_end)
		inspect_archive_end(path, state, *parsed.archive_end, bytes.size());
}

void inspect_spool(const Options &opts) {
	require(fs::is_directory(opts.spool_dir),
	    format("{} is not a directory", opts.spool_dir.string()));

	InspectState state;
	for (const fs::path &volume_dir : sorted_dirs(opts.spool_dir)) {
		for (const fs::path &file : sorted_files(volume_dir)) {
			inspect_file(file, state);
			if (state.saw_archive_end)
				break;
		}
		if (state.saw_archive_end)
			break;
	}
	require(state.saw_archive_end, "missing Archive End Header");
	std::cout << format("ok: archive {} volumes={} slices={} frames={}\n",
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
