#include "neotape/common.hpp"
#include <charconv>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>

namespace neotape {

bool g_debug = false;

namespace {

std::mutex diagnostic_mutex;
bool progress_active = false;

} // namespace

using std::format;
using std::size;
using std::string;
using std::string_view;

void write_diagnostic(string_view message) {
    std::scoped_lock const lock(diagnostic_mutex);
    if (progress_active) {
        std::cerr << '\n';
        progress_active = false;
    }
    std::cerr << message;
    if (!message.empty() && message.back() != '\n') {
        std::cerr << '\n';
    }
}

void write_progress(string_view message) {
    std::scoped_lock const lock(diagnostic_mutex);
    std::cerr << '\r' << message;
    progress_active = true;
}

void finish_progress() {
    std::scoped_lock const lock(diagnostic_mutex);
    if (progress_active) {
        std::cerr << '\n';
        progress_active = false;
    }
}

string escape_bytes_for_diagnostic(string_view bytes) {
    string escaped;
    escaped.reserve(bytes.size());
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char const byte : bytes) {
        if (byte >= 0x20 && byte <= 0x7e && byte != '\\') {
            escaped.push_back(static_cast<char>(byte));
            continue;
        }
        if (byte == '\\') {
            escaped += "\\\\";
            continue;
        }
        escaped += "\\x";
        escaped.push_back(hex[byte >> 4]);
        escaped.push_back(hex[byte & 0x0f]);
    }
    return escaped;
}

bool locale_name_is_utf8(const char *name) {
    if (name == nullptr) {
        return false;
    }
    string locale_name(name);
    std::ranges::transform(
        locale_name, locale_name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return locale_name.find("utf-8") != string::npos ||
           locale_name.find("utf8") != string::npos;
}

string hex_encode(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    string result;
    result.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

uint64_t parse_uint(string_view text, string_view name, uint64_t minimum,
                    uint64_t maximum) {
    uint64_t value = 0;
    if (text.empty()) {
        throw std::invalid_argument(format("{} is empty", name));
    }
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument(format("invalid {}: {}", name, text));
    }
    if (value < minimum || value > maximum) {
        throw std::out_of_range(
            format("{} must be from {} to {}", name, minimum, maximum));
    }
    return value;
}

uint64_t parse_size(string_view text, string_view name, uint64_t maximum) {
    if (text.empty()) {
        throw std::invalid_argument(format("{} is empty", name));
    }

    uint64_t multiplier = 1;
    char const suffix = text.back();
    if (suffix == 'k' || suffix == 'K' || suffix == 'm' || suffix == 'M' ||
        suffix == 'g' || suffix == 'G' || suffix == 't' || suffix == 'T') {
        text.remove_suffix(1);
        switch (suffix) {
        case 'k':
        case 'K':
            multiplier = 1024ULL;
            break;
        case 'm':
        case 'M':
            multiplier = 1024ULL * 1024;
            break;
        case 'g':
        case 'G':
            multiplier = 1024ULL * 1024 * 1024;
            break;
        case 't':
        case 'T':
            multiplier = 1024ULL * 1024 * 1024 * 1024;
            break;
        }
    }

    return parse_uint(text, name, 1, maximum / multiplier) * multiplier;
}

string humanize_number(std::size_t number) {
    constexpr const char *suffixes[] = {"", "K", "M", "G", "T", "P", "E"};

    auto value = static_cast<double>(number);
    std::size_t suffix_index = 0;
    while (value >= 1024.0 && suffix_index + 1 < size(suffixes)) {
        value /= 1024.0;
        ++suffix_index;
    }

    if (suffix_index == 0) {
        return format("{}", number);
    }
    if (value < 10.0) {
        return format("{:.1f}{}", value, suffixes[suffix_index]);
    }
    return format("{:.0f}{}", std::round(value), suffixes[suffix_index]);
}

void ensure_utf8_ctype_locale() {
    const char *locale_name = std::setlocale(LC_CTYPE, "");
    if (locale_name_is_utf8(locale_name)) {
        return;
    }
    for (const char *fallback : {"C.UTF-8", "en_US.UTF-8"}) {
        locale_name = std::setlocale(LC_CTYPE, fallback);
        if (locale_name_is_utf8(locale_name)) {
            return;
        }
    }
}

namespace fs = std::filesystem;

bool has_trailing_slash(string_view path) {
    return path.size() > 1 && path.back() == '/';
}

string strip_trailing_slashes(string_view path) {
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    return string(path);
}

SourceSpec make_source_spec(const string &arg) {
    SourceSpec spec;

    string cleaned = strip_trailing_slashes(arg);
    spec.archive_prefix = cleaned;
    if (!spec.archive_prefix.empty() && spec.archive_prefix[0] == '/') {
        spec.archive_prefix.erase(0, 1);
    }

    spec.open_path = fs::absolute(cleaned).lexically_normal();

    struct stat st{};
    if (lstat(spec.open_path.c_str(), &st) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                format("lstat {}", spec.open_path.string()));
    }

    return spec;
}

string archive_path_for_source(const SourceSpec &spec,
                               const string &source_path) {
    fs::path const absolute =
        fs::absolute(fs::path(source_path)).lexically_normal();
    fs::path const relative = absolute.lexically_relative(spec.open_path);
    if (relative.empty() || relative == ".") {
        return spec.archive_prefix;
    }
    return (fs::path(spec.archive_prefix) / relative).generic_string();
}

} // namespace neotape
