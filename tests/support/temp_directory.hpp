#pragma once

#include <filesystem>

namespace neotape::test {

class TemporaryDirectory {
  public:
    TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    ~TemporaryDirectory();

    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

} // namespace neotape::test
