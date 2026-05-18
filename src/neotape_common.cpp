#include "neotape/common.hpp"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iterator>
#include <limits>
#include <stdexcept>

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

} // namespace neotape
