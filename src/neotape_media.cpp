#include "neotape/format.hpp"
#include "neotape/media.hpp"

#include <algorithm>
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

std::optional<SpoolFileName> spool_file_name(const fs::path &path) {
    std::string const name = path.filename().string();
    constexpr std::string_view prefix = "neotape-";
    if (!name.starts_with(prefix) || !name.ends_with(".nts"))
        return std::nullopt;
    std::string_view rest(name.data() + prefix.size(),
                          name.size() - prefix.size() - 4);
    auto number = [](std::string_view &text) -> std::optional<uint64_t> {
        auto end = text.find_first_not_of("0123456789");
        if (end == std::string_view::npos)
            end = text.size();
        if (end == 0)
            return std::nullopt;
        uint64_t result = 0;
        auto [ptr, error] =
            std::from_chars(text.data(), text.data() + end, result);
        if (error == std::errc::result_out_of_range)
            throw std::runtime_error("spool filename number overflow");
        if (error != std::errc{})
            return std::nullopt;
        text.remove_prefix(end);
        return result;
    };
    auto file = number(rest);
    if (!file || !rest.starts_with("."))
        return std::nullopt;
    rest.remove_prefix(1);
    std::optional<uint64_t> slice;
    if (rest.starts_with("slice-")) {
        rest.remove_prefix(6);
        slice = number(rest);
        if (!slice)
            return std::nullopt;
    } else if (rest.starts_with("archive-end")) {
        rest.remove_prefix(11);
    } else {
        return std::nullopt;
    }
    if (!rest.empty()) {
        if (!rest.starts_with("-") || rest.size() == 1)
            return std::nullopt;
        rest.remove_prefix(1);
        for (char c : rest)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return std::nullopt;
    }
    return SpoolFileName{*file, slice};
}

std::vector<fs::path> scan_spool_files(const fs::path &root) {
    if (!fs::is_directory(root))
        throw std::runtime_error("spool directory does not exist: " +
                                 root.string());
    std::vector<std::pair<uint64_t, fs::path>> numbered;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        if (auto const name = spool_file_name(entry.path()))
            numbered.emplace_back(name->number, entry.path());
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
