#include "neotape/format.hpp"

#include <blake3.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ====================== Writer State =============================

namespace fs = std::filesystem;
using std::format;
using std::size_t;
using std::string;
using std::string_view;
using std::vector;

struct Options {
	string input = "-";
	fs::path output_dir;
	string archive_name = "raw";
	uint32_t volume_block_size = 1024 * 1024;
	uint64_t slice_size = 64ull * 1024 * 1024;
	uint64_t virtual_volume_size = 0;
};

struct WriterState {
	Options opts;
	string archive_uuid;
	uint64_t volume_seq_num = 0;
	uint64_t tape_file_num = 0;
	uint64_t volume_used = 0;
	uint64_t logical_slice_seq_num = 0;
	uint64_t global_frame_seq_num = 0;
	uint64_t frame_seq_num_within_slice = 0;
	uint64_t current_slice_size = 0;
	bool slice_open = false;
	blake3_hasher slice_hasher;
	fs::path current_volume_dir;
	fs::path current_slice_path;
	vector<string> manifest_files;
};

// ====================== Diagnostics & CLI ========================

[[noreturn]] void fail(const string &message) {
	std::cerr << format("neotape-write: {}\n", message);
	std::exit(1);
}

[[noreturn]] void fail_errno(const string &context) {
	fail(format("{}: {}", context, std::strerror(errno)));
}

void usage(const char *prog) {
	std::cerr << format(
	    "usage: {} --target=spool -o <spool-dir> [-f <input|->] "
	    "[--archive-name <name>] [--volume-block-size <bytes>] "
	    "[--slice-size <bytes>] [--virtual-volume-size <bytes>]\n",
	    prog);
}

uint64_t parse_u64(string_view text, const char *name) {
	if (text.empty())
		fail(format("{} is empty", name));
	string owned(text);
	char *end = nullptr;
	errno = 0;
	unsigned long long value = std::strtoull(owned.c_str(), &end, 10);
	if (errno != 0 || end == nullptr || *end != '\0')
		fail(format("invalid {}: {}", name, text));
	return static_cast<uint64_t>(value);
}

Options parse_args(int argc, char **argv) {
	Options opts;
	bool saw_target = false;

	for (int i = 1; i < argc; ++i) {
		string_view arg(argv[i]);
		auto need_value = [&](const char *name) -> string {
			if (++i >= argc)
				fail(format("{} requires a value", name));
			return argv[i];
		};

		if (arg == "--target=spool") {
			saw_target = true;
		} else if (arg == "--target") {
			string value = need_value("--target");
			if (value != "spool")
				fail("only --target=spool is supported");
			saw_target = true;
		} else if (arg == "-f") {
			opts.input = need_value("-f");
		} else if (arg == "-o") {
			opts.output_dir = need_value("-o");
		} else if (arg == "--archive-name") {
			opts.archive_name = need_value("--archive-name");
		} else if (arg == "--volume-block-size") {
			opts.volume_block_size = static_cast<uint32_t>(
			    parse_u64(need_value("--volume-block-size"), "volume block size"));
		} else if (arg == "--slice-size") {
			opts.slice_size = parse_u64(need_value("--slice-size"), "slice size");
		} else if (arg == "--virtual-volume-size") {
			opts.virtual_volume_size =
			    parse_u64(need_value("--virtual-volume-size"), "virtual volume size");
		} else if (arg == "-h" || arg == "--help") {
			usage(argv[0]);
			std::exit(0);
		} else {
			fail(format("unknown option: {}", arg));
		}
	}

	if (!saw_target || opts.output_dir.empty()) {
		usage(argv[0]);
		std::exit(2);
	}
	if (!neotape::valid_block_size(opts.volume_block_size))
		fail("volume block size must be between 4096 and 16777216 bytes");
	if (opts.slice_size == 0)
		fail("slice size must be greater than zero");
	if (opts.virtual_volume_size != 0 &&
	    opts.virtual_volume_size < static_cast<uint64_t>(opts.volume_block_size) * 2)
		fail("virtual volume size must fit at least a volume header and one record");
	return opts;
}

string six(uint64_t value) {
	return format("{:06}", value);
}

// ====================== Manifest Helpers =========================

string json_escape(string_view text) {
	string out;
	for (char c : text) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

fs::path tape_file_path(WriterState &state, string_view suffix) {
	++state.tape_file_num;
	return state.current_volume_dir /
	       format("tape-file-{}.{}.ntf", six(state.tape_file_num), suffix);
}

// ====================== Record Files =============================

void write_record(const fs::path &path,
    const neotape::HeaderBytes &header, const vector<uint8_t> *payload,
    uint32_t block_size, bool append) {
	std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::openmode{}));
	if (!out)
		fail(format("open {}", path.string()));
	out.write(reinterpret_cast<const char *>(header.data()), header.size());
	if (!out)
		fail(format("write {}", path.string()));
	size_t payload_size = payload == nullptr ? 0 : payload->size();
	if (payload != nullptr && payload_size > 0)
		out.write(reinterpret_cast<const char *>(payload->data()), payload_size);
	if (!out)
		fail(format("write {}", path.string()));

	vector<char> zeros(64 * 1024, 0);
	uint64_t remaining = block_size - neotape::fixed_header_size - payload_size;
	while (remaining > 0) {
		size_t n = static_cast<size_t>(
		    std::min<uint64_t>(remaining, zeros.size()));
		out.write(zeros.data(), n);
		if (!out)
			fail(format("write {}", path.string()));
		remaining -= n;
	}
}

void append_manifest_file(WriterState &state, const fs::path &path,
    uint64_t size, const neotape::Hash &hash) {
	state.manifest_files.push_back(format(
	    "    {{\"volume_seq_num\":{},\"tape_file_num\":{},\"path\":\"{}\","
	    "\"size\":{},\"blake3\":\"{}\"}}",
	    state.volume_seq_num, state.tape_file_num,
	    json_escape(path.lexically_relative(state.opts.output_dir).generic_string()), size,
	    neotape::hash_hex(hash)));
}

neotape::Hash hash_file(const fs::path &path);

// ====================== Volume Management ========================

void finalize_current_slice_file(WriterState &state) {
	if (state.current_slice_path.empty())
		return;
	uint64_t size = fs::file_size(state.current_slice_path);
	append_manifest_file(state, state.current_slice_path, size,
	    hash_file(state.current_slice_path));
	state.current_slice_path.clear();
}

neotape::Hash hash_file(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		fail(format("open {}", path.string()));
	vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
	    std::istreambuf_iterator<char>());
	return neotape::blake3_hash(bytes.data(), bytes.size());
}

void write_volume_header(WriterState &state) {
	++state.volume_seq_num;
	state.tape_file_num = 0;
	state.volume_used = 0;
	state.current_volume_dir =
	    state.opts.output_dir / format("volume-{}", six(state.volume_seq_num));
	fs::create_directories(state.current_volume_dir);

	neotape::VolumeHeader header;
	header.volume_block_size = state.opts.volume_block_size;
	header.archive_uuid = state.archive_uuid;
	header.archive_name = state.opts.archive_name;
	header.volume_seq_num = state.volume_seq_num;
	header.volume_write_at_utc = neotape::utc_timestamp_now();

	fs::path path = tape_file_path(state, "volume-header");
	write_record(path, neotape::serialize_volume_header(header), nullptr,
	    state.opts.volume_block_size, false);
	state.volume_used += state.opts.volume_block_size;
	append_manifest_file(state, path, state.opts.volume_block_size, hash_file(path));
}

void ensure_room_for_record(WriterState &state) {
	if (state.opts.virtual_volume_size == 0)
		return;
	if (state.volume_used + state.opts.volume_block_size <= state.opts.virtual_volume_size)
		return;
	finalize_current_slice_file(state);
	write_volume_header(state);
}

// ====================== Content Framing ==========================

void write_content_frame(WriterState &state, const vector<uint8_t> &payload,
    bool end) {
	ensure_room_for_record(state);

	if (!state.slice_open) {
		++state.logical_slice_seq_num;
		state.frame_seq_num_within_slice = 0;
		state.current_slice_size = 0;
		state.slice_open = true;
		blake3_hasher_init(&state.slice_hasher);
	}
	if (state.current_slice_path.empty())
		state.current_slice_path =
		    tape_file_path(state, format("slice-{}", six(state.logical_slice_seq_num)));

	++state.global_frame_seq_num;
	++state.frame_seq_num_within_slice;
	neotape::Hash payload_hash = neotape::blake3_hash(payload.data(), payload.size());
	blake3_hasher_update(&state.slice_hasher, payload.data(), payload.size());
	state.current_slice_size += payload.size();

	neotape::Hash slice_hash{};
	if (end)
		blake3_hasher_finalize(&state.slice_hasher, slice_hash.data(), slice_hash.size());

	// Only the terminal frame of a logical slice carries the whole-slice
	// digest. Earlier frames stay independently verifiable by payload hash.
	neotape::FrameHeader header;
	header.volume_block_size = state.opts.volume_block_size;
	header.archive_uuid = state.archive_uuid;
	header.archive_name = state.opts.archive_name;
	header.volume_seq_num = state.volume_seq_num;
	header.logical_slice_seq_num = state.logical_slice_seq_num;
	header.global_frame_seq_num = state.global_frame_seq_num;
	header.frame_seq_num_within_slice = state.frame_seq_num_within_slice;
	header.frame_payload_size = payload.size();
	header.frame_payload_blake3 = payload_hash;
	uint16_t flags = 0;
	if (state.frame_seq_num_within_slice == 1)
		flags |= neotape::frame_flag_start;
	if (end) {
		header.slice_content_size = state.current_slice_size;
		header.slice_content_blake3 = slice_hash;
		flags |= neotape::frame_flag_end;
	}
	header.flags = flags;

	write_record(state.current_slice_path, neotape::serialize_frame_header(header),
	    &payload, state.opts.volume_block_size, true);
	state.volume_used += state.opts.volume_block_size;

	if (end) {
		state.slice_open = false;
		finalize_current_slice_file(state);
	}
}

void write_archive_end(WriterState &state) {
	ensure_room_for_record(state);

	neotape::ArchiveEndHeader header;
	header.volume_block_size = state.opts.volume_block_size;
	header.archive_uuid = state.archive_uuid;
	header.archive_name = state.opts.archive_name;
	header.volume_seq_num = state.volume_seq_num;
	header.last_logical_slice_seq_num = state.logical_slice_seq_num;
	header.last_global_frame_seq_num = state.global_frame_seq_num;
	header.created_by_implementation = "NeoTape reference writer phase2-mvp";
	header.archive_end_at_utc = neotape::utc_timestamp_now();

	fs::path path = tape_file_path(state, "archive-end");
	write_record(path, neotape::serialize_archive_end_header(header), nullptr,
	    state.opts.volume_block_size, false);
	state.volume_used += state.opts.volume_block_size;
	append_manifest_file(state, path, state.opts.volume_block_size, hash_file(path));
}

// ====================== Spool Writer Pipeline ====================

void write_manifest(const WriterState &state) {
	fs::path path = state.opts.output_dir / "manifest.json";
	std::ofstream out(path);
	if (!out)
		fail(format("open {}", path.string()));

	out << "{\n";
	out << format("  \"archive_uuid\":\"{}\",\n", state.archive_uuid);
	out << format("  \"archive_name\":\"{}\",\n", json_escape(state.opts.archive_name));
	out << "  \"writer\":\"NeoTape reference writer phase2-mvp\",\n";
	out << "  \"target_backend\":\"spool\",\n";
	out << "  \"payload_profile\":\"raw\",\n";
	out << format("  \"volume_block_size\":{},\n", state.opts.volume_block_size);
	out << format("  \"slice_size\":{},\n", state.opts.slice_size);
	out << format("  \"virtual_volume_size\":{},\n", state.opts.virtual_volume_size);
	out << format("  \"volume_count\":{},\n", state.volume_seq_num);
	out << "  \"files\":[\n";
	for (size_t i = 0; i < state.manifest_files.size(); ++i) {
		out << state.manifest_files[i];
		if (i + 1 != state.manifest_files.size())
			out << ",";
		out << "\n";
	}
	out << "  ]\n";
	out << "}\n";
}

void write_spool_archive(const Options &opts) {
	if (fs::exists(opts.output_dir))
		fail(format("output directory already exists: {}", opts.output_dir.string()));
	fs::create_directories(opts.output_dir);

	FILE *input = stdin;
	if (opts.input != "-") {
		input = std::fopen(opts.input.c_str(), "rb");
		if (input == nullptr)
			fail_errno(string("open ") + opts.input);
	}

	WriterState state;
	state.opts = opts;
	state.archive_uuid = neotape::make_uuid_v4();
	write_volume_header(state);

	size_t frame_payload_capacity = opts.volume_block_size - neotape::fixed_header_size;
	vector<uint8_t> buffer(frame_payload_capacity);
	vector<uint8_t> pending;
	bool have_pending = false;
	for (;;) {
		// Keep one frame pending so an exact slice boundary can be marked END
		// without emitting an empty sentinel frame.
		if (have_pending &&
		    state.current_slice_size + pending.size() >= opts.slice_size) {
			write_content_frame(state, pending, true);
			pending.clear();
			have_pending = false;
			continue;
		}

		uint64_t pending_size = have_pending ? pending.size() : 0;
		uint64_t remaining_in_slice =
		    state.slice_open ? opts.slice_size - state.current_slice_size - pending_size
				     : opts.slice_size - pending_size;
		size_t want = static_cast<size_t>(
		    std::min<uint64_t>(buffer.size(), remaining_in_slice));
		size_t n = std::fread(buffer.data(), 1, want, input);
		if (n > 0) {
			if (have_pending) {
				write_content_frame(state, pending, false);
				pending.clear();
				have_pending = false;
			}
			vector<uint8_t> payload(buffer.begin(),
			    buffer.begin() + static_cast<std::ptrdiff_t>(n));
			if (n != want) {
				write_content_frame(state, payload, true);
			} else {
				pending = std::move(payload);
				have_pending = true;
			}
		}
		if (n != want) {
			if (std::ferror(input))
				fail_errno("read input");
			break;
		}
	}

	if (input != stdin && std::fclose(input) != 0)
		fail_errno(string("close ") + opts.input);

	if (have_pending)
		write_content_frame(state, pending, true);
	write_archive_end(state);
	write_manifest(state);
	std::cerr << format("archive {} written to {}\n", state.archive_uuid,
	    opts.output_dir.string());
}

} // namespace

int main(int argc, char **argv) {
	Options opts = parse_args(argc, argv);
	write_spool_archive(opts);
	return 0;
}
