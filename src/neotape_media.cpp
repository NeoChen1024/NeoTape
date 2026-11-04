#include "neotape/format.hpp"
#include "neotape/media.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <unistd.h>

namespace neotape {
namespace fs = std::filesystem;
using std::format;

MediaLocator parse_media(std::string_view text, bool allow_null) {
    if (allow_null && text == "null")
        return {MediaLocator::null_sink, {}};
    for (auto [prefix, kind] : {std::pair{"tape:", MediaLocator::tape},
                                {"spool:", MediaLocator::spool}}) {
        if (text.starts_with(prefix) && text.size() > std::strlen(prefix))
            return {kind, std::string(text.substr(std::strlen(prefix)))};
    }
    throw std::invalid_argument(
        allow_null ? "target must be tape:<device>, spool:<dir>, or null"
                   : "source must be tape:<device> or spool:<dir>");
}

FrameHeader read_spool_file_header(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    HeaderBytes bytes{};
    if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        throw std::runtime_error(
            format("short header read from {}", path.string()));
    return parse_fixed_header(bytes.data(), bytes.size());
}

std::optional<uint64_t> spool_file_number(const fs::path &path) {
    std::string name = path.filename().string();
    std::ranges::transform(name, name.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    // NT + 1..6 digits + .NTS: an 8.3 name, so it fits FAT without long
    // names and ISO 9660 Level 1.
    if (name.size() < 7 || name.size() > 12 || !name.starts_with("NT") ||
        !name.ends_with(".NTS"))
        return std::nullopt;
    std::string_view const digits(name.data() + 2, name.size() - 6);
    if (digits.find_first_not_of("0123456789") != std::string_view::npos)
        return std::nullopt;
    uint64_t number = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), number);
    return number;
}

std::vector<fs::path> scan_spool_files(const fs::path &root) {
    if (!fs::is_directory(root))
        throw std::runtime_error("spool directory does not exist: " +
                                 root.string());
    std::vector<std::pair<uint64_t, fs::path>> numbered;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        if (auto const number = spool_file_number(entry.path()))
            numbered.emplace_back(*number, entry.path());
    }
    std::ranges::sort(numbered);
    for (std::size_t i = 1; i < numbered.size(); ++i) {
        auto const [previous, previous_path] = numbered[i - 1];
        auto const [current, current_path] = numbered[i];
        if (previous == current)
            throw std::runtime_error(
                format("duplicate spool file number {}: {} and {}", current,
                       previous_path.filename().string(),
                       current_path.filename().string()));
        if (current - previous > 1)
            std::cerr << format(
                "neotape: missing spool file numbers between {} and {}\n",
                previous, current);
    }
    std::vector<fs::path> files;
    files.reserve(numbered.size());
    for (auto &[number, path] : numbered)
        files.push_back(std::move(path));
    return files;
}

RecordReader::RecordReader(const MediaLocator &source) {
    if (source.kind == MediaLocator::tape) {
        auto tape = std::make_unique<mt::TapeDevice>(source.path, false);
        // The device was opened without waiting for the drive; reads block.
        int flags = ::fcntl(tape->fd(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(tape->fd(), F_SETFL, flags & ~O_NONBLOCK) < 0)
            throw std::runtime_error(format("fcntl: {}", std::strerror(errno)));
        // Drives without variable block mode can still be read when their
        // fixed block size matches the recorded block size.
        try {
            tape->set_variable_block_mode();
        } catch (const mt::Error &e) {
            std::cerr << format(
                "neotape: warning: variable block mode unavailable: {}\n",
                e.what());
        }
        device_ = std::move(tape);
    } else if (source.kind == MediaLocator::spool) {
        device_ = std::make_unique<mt::SpoolTapeReader>(source.path);
    } else
        throw std::invalid_argument("record reader requires tape or spool");
}

MediaRecord RecordReader::next() {
    if (ended_)
        return {};
    MediaRecord record = device_->read_record();
    ended_ = record.event == RecordEvent::end;
    return record;
}

void RecordReader::skip_file() {
    if (ended_)
        return;
    try {
        device_->space_fwd();
    } catch (const mt::Error &) {
        if (!device_->status().eod())
            throw;
        ended_ = true;
    }
}
} // namespace neotape
