#include "process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace neotape::test {
namespace {

using namespace std::chrono_literals;

void close_fd(int fd) {
    if (fd >= 0) {
        while (::close(fd) < 0 && errno == EINTR) {
        }
    }
}

void set_nonblocking(int fd) {
    int const flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("fcntl failed: " + std::string(std::strerror(errno)));
    }
}

void apply_limit(int resource, const std::optional<std::size_t> &value) {
    if (!value.has_value()) {
        return;
    }
    rlimit limit{static_cast<rlim_t>(*value), static_cast<rlim_t>(*value)};
    if (::setrlimit(resource, &limit) != 0) {
        _exit(126);
    }
}

void append_available(int fd, std::string &destination) {
    std::array<char, 8192> buffer{};
    for (;;) {
        ssize_t const count = ::read(fd, buffer.data(), buffer.size());
        if (count > 0) {
            destination.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

int exit_code_from_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

} // namespace

Process::Process(ProcessOptions options) {
    if (options.arguments.empty()) {
        throw std::invalid_argument("a process requires an executable");
    }

    int stdout_pipe[2];
    int stderr_pipe[2];
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        throw std::runtime_error("pipe failed: " + std::string(std::strerror(errno)));
    }

    pid_ = ::fork();
    if (pid_ < 0) {
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[0]);
        close_fd(stderr_pipe[1]);
        throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    }

    if (pid_ == 0) {
        close_fd(stdout_pipe[0]);
        close_fd(stderr_pipe[0]);
        if (::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[1]);

        if (options.working_directory.has_value() &&
            ::chdir(options.working_directory->c_str()) != 0) {
            _exit(126);
        }
        for (const auto &[name, value] : options.environment) {
            if (::setenv(name.c_str(), value.c_str(), 1) != 0) {
                _exit(126);
            }
        }
        apply_limit(RLIMIT_AS, options.address_space_limit);
        apply_limit(RLIMIT_NOFILE, options.open_file_limit);

        std::vector<char *> argv;
        argv.reserve(options.arguments.size() + 1);
        for (std::string &argument : options.arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        ::execv(argv.front(), argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[1]);
    stdout_fd_ = stdout_pipe[0];
    stderr_fd_ = stderr_pipe[0];
    set_nonblocking(stdout_fd_);
    set_nonblocking(stderr_fd_);
}

Process::~Process() {
    terminate();
    close_pipes();
}

bool Process::running() {
    if (pid_ < 0 || wait_status_.has_value()) {
        return false;
    }
    int status = 0;
    pid_t const result = ::waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
        wait_status_ = status;
        drain_output();
        return false;
    }
    if (result < 0 && errno != EINTR) {
        throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
    }
    drain_output();
    return true;
}

ProcessResult Process::wait(std::chrono::milliseconds timeout) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }

    bool const timed_out = running();
    if (timed_out) {
        terminate();
    }
    drain_output();
    return ProcessResult{wait_status_.has_value()
                             ? exit_code_from_status(*wait_status_)
                             : -1,
                         timed_out, stdout_, stderr_};
}

void Process::terminate() {
    if (!running()) {
        return;
    }
    ::kill(pid_, SIGTERM);
    auto const deadline = std::chrono::steady_clock::now() + 500ms;
    while (running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    if (running()) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        wait_status_ = status;
        drain_output();
    }
}

ProcessResult Process::run(ProcessOptions options,
                           std::chrono::milliseconds timeout) {
    Process process(std::move(options));
    return process.wait(timeout);
}

void Process::drain_output() {
    append_available(stdout_fd_, stdout_);
    append_available(stderr_fd_, stderr_);
}

void Process::close_pipes() {
    close_fd(stdout_fd_);
    close_fd(stderr_fd_);
    stdout_fd_ = -1;
    stderr_fd_ = -1;
}

bool wait_for_unix_socket(const std::filesystem::path &path, Process &owner,
                          std::chrono::milliseconds timeout) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        struct stat status {};
        if (::stat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode)) {
            return true;
        }
        if (!owner.running()) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

} // namespace neotape::test
