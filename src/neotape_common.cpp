#include "neotape/common.hpp"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace neotape {

using std::string;
using std::string_view;
using std::format;
using std::size;

uint64_t parse_size(string_view text, string_view name) {
	if (text.empty())
		throw std::invalid_argument(format("{} is empty", name));

	uint64_t multiplier = 1;
	char suffix = text.back();
	if (suffix == 'k' || suffix == 'K' || suffix == 'm' || suffix == 'M' ||
	    suffix == 'g' || suffix == 'G' || suffix == 't' || suffix == 'T') {
		text.remove_suffix(1);
		switch (suffix) {
		case 'k':
		case 'K':
			multiplier = 1024ull;
			break;
		case 'm':
		case 'M':
			multiplier = 1024ull * 1024;
			break;
		case 'g':
		case 'G':
			multiplier = 1024ull * 1024 * 1024;
			break;
		case 't':
		case 'T':
			multiplier = 1024ull * 1024 * 1024 * 1024;
			break;
		}
	}

	string owned(text);
	char *end = nullptr;
	errno = 0;
	unsigned long long value = std::strtoull(owned.c_str(), &end, 10);
	if (errno != 0 || end == nullptr || *end != '\0')
		throw std::invalid_argument(format("invalid {}: {}", name, text));
	if (value == 0)
		throw std::invalid_argument(format("{} must be greater than zero", name));
	if (value > std::numeric_limits<uint64_t>::max() / multiplier)
		throw std::overflow_error(format("{} is too large", name));
	return static_cast<uint64_t>(value) * multiplier;
}

string humanize_number(std::size_t number) {
	constexpr const char *suffixes[] = {"", "K", "M", "G", "T", "P", "E"};

	double value = static_cast<double>(number);
	std::size_t suffix_index = 0;
	while (value >= 1024.0 && suffix_index + 1 < size(suffixes)) {
		value /= 1024.0;
		++suffix_index;
	}

	if (suffix_index == 0)
		return format("{}", number);
	if (value < 10.0)
		return format("{:.1f}{}", value, suffixes[suffix_index]);
	return format("{:.0f}{}", std::round(value), suffixes[suffix_index]);
}

namespace fs = std::filesystem;

bool has_trailing_slash(string_view path) {
	return path.size() > 1 && path.back() == '/';
}

string strip_trailing_slashes(string_view path) {
	while (path.size() > 1 && path.back() == '/')
		path.remove_suffix(1);
	return string(path);
}

SourceSpec make_source_spec(const string &arg) {
	SourceSpec spec;
	spec.original = arg;

	string stripped = strip_trailing_slashes(arg);
	bool follow_top_symlink = has_trailing_slash(arg);
	fs::path display_path = fs::absolute(stripped).lexically_normal();

	std::error_code ec;
	if (follow_top_symlink) {
		fs::file_status status = fs::status(display_path, ec);
		if (ec)
			throw std::system_error(ec, format("stat {}", arg));
		if (!fs::is_directory(status))
			throw std::runtime_error(format("source is not a directory: {}", arg));
		spec.open_path = fs::canonical(display_path, ec);
		if (ec)
			throw std::system_error(ec, format("canonical {}", arg));
	} else {
		fs::file_status status = fs::symlink_status(display_path, ec);
		if (ec)
			throw std::system_error(ec, format("lstat {}", arg));
		if (!fs::exists(status))
			throw std::runtime_error(format("source does not exist: {}", arg));
		spec.open_path = display_path;
	}

	spec.open_parent = spec.open_path.parent_path();
	spec.archive_root = display_path.filename();
	if (spec.archive_root.empty())
		spec.archive_root = ".";
	return spec;
}

fs::path drop_first_component(const fs::path &path) {
	fs::path out;
	bool first = true;
	for (const auto &component : path) {
		if (first) {
			first = false;
			continue;
		}
		out /= component;
	}
	return out;
}

string archive_path_for_source(const SourceSpec &spec, const string &source_path) {
	fs::path absolute = fs::absolute(fs::path(source_path)).lexically_normal();
	fs::path relative = absolute.lexically_relative(spec.open_parent);
	fs::path path_in_archive = spec.archive_root;
	fs::path child_path =
	    spec.archive_root == "." ? relative : drop_first_component(relative);
	if (!child_path.empty())
		path_in_archive /= child_path;
	return path_in_archive.generic_string();
}

} // namespace neotape
