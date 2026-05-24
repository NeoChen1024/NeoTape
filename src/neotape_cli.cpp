#include "neotape/cli.hpp"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace neotape {

Locator parse_locator(std::string_view text) {
    auto pos = text.find(':');
    if (pos == std::string_view::npos || pos == 0 || pos + 1 == text.size())
        throw std::invalid_argument("locator must be <kind>:<locator>");
    return Locator{std::string(text.substr(0, pos)),
                   std::string(text.substr(pos + 1))};
}

ControlPolicy parse_control_policy(std::string_view text) {
    if (text == "auto")
        return ControlPolicy::auto_prompt;
    if (text == "none")
        return ControlPolicy::none;
    throw std::invalid_argument("control policy must be auto or none");
}

std::string control_policy_name(ControlPolicy policy) {
    switch (policy) {
    case ControlPolicy::auto_prompt:
        return "auto";
    case ControlPolicy::none:
        return "none";
    }
    return "unknown";
}

void require_prompt_allowed(ControlPolicy policy) {
    if (policy == ControlPolicy::none)
        throw std::runtime_error(
            "volume change required but --control=none is set");
}

namespace {

int open_tty() { return ::open("/dev/tty", O_RDWR | O_CLOEXEC); }

void write_all(int fd, std::string_view text) {
    const char *p = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0)
            throw std::runtime_error("write /dev/tty failed");
        p += n;
        left -= static_cast<std::size_t>(n);
    }
}

std::string read_line(int fd) {
    std::string out;
    char ch = 0;
    while (true) {
        ssize_t n = ::read(fd, &ch, 1);
        if (n <= 0)
            throw std::runtime_error("read /dev/tty failed");
        if (ch == '\n')
            break;
        out.push_back(ch);
    }
    return out;
}

} // namespace

VolumePromptResult prompt_for_volume_change(const VolumePromptRequest &request) {
    int tty = open_tty();
    if (tty < 0)
        throw std::runtime_error("volume change requires /dev/tty");

    auto close_tty = [&]() {
        if (tty >= 0) {
            ::close(tty);
            tty = -1;
        }
    };

    try {
        while (true) {
            write_all(tty, std::format(
                               "Volume change required for archive {}, expected volume {}.\n"
                               "Options:\n"
                               "  [c] Continue\n"
                               "  [d] Change device\n"
                               "  [s] Shell\n"
                               "  [a] Abort\n"
                               "> ",
                               request.archive_uuid, request.expected_volume));
            std::string line = read_line(tty);
            if (line == "c" || line == "C") {
                close_tty();
                return VolumePromptResult{VolumePromptChoice::continue_current,
                                          std::nullopt};
            }
            if (line == "d" || line == "D") {
                write_all(tty, "New locator: ");
                auto locator = parse_locator(read_line(tty));
                close_tty();
                return VolumePromptResult{VolumePromptChoice::change_locator,
                                          locator};
            }
            if (line == "s" || line == "S") {
                const char *shell = std::getenv("SHELL");
                if (shell == nullptr || std::strlen(shell) == 0)
                    shell = "/bin/sh";
                write_all(tty,
                          std::format("Entering shell {}. Exit to return.\n",
                                      shell));
                int rc = std::system(shell);
                write_all(tty, std::format("Shell exited with status {}.\n", rc));
                continue;
            }
            if (line == "a" || line == "A") {
                close_tty();
                return VolumePromptResult{VolumePromptChoice::abort,
                                          std::nullopt};
            }
            write_all(tty, "Choose c, d, s, or a.\n");
        }
    } catch (...) {
        close_tty();
        throw;
    }
}

} // namespace neotape
