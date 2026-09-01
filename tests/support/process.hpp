#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include <sys/types.h>

namespace neotape::test {

struct ProcessOptions {
    explicit ProcessOptions(std::vector<std::string> process_arguments)
        : arguments(std::move(process_arguments)) {}

    std::vector<std::string> arguments;
    std::optional<std::filesystem::path> working_directory;
    std::vector<std::pair<std::string, std::string>> environment;
    std::optional<std::size_t> address_space_limit;
    std::optional<std::size_t> open_file_limit;
};

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string standard_output;
    std::string standard_error;
};

class Process {
  public:
    explicit Process(ProcessOptions options);
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;
    Process(Process &&) = delete;
    Process &operator=(Process &&) = delete;
    ~Process();

    [[nodiscard]] bool running();
    ProcessResult wait(std::chrono::milliseconds timeout);
    void terminate();

    static ProcessResult run(ProcessOptions options,
                             std::chrono::milliseconds timeout);

  private:
    void drain_output();
    void close_pipes();

    pid_t pid_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    std::optional<int> wait_status_;
    std::string stdout_;
    std::string stderr_;
};

bool wait_for_unix_socket(const std::filesystem::path &path, Process &owner,
                          std::chrono::milliseconds timeout);

} // namespace neotape::test
