#include "temp_directory.hpp"

#include <array>
#include <stdexcept>

#include <cstdlib>

namespace neotape::test {

TemporaryDirectory::TemporaryDirectory() {
    std::array<char, 64> pattern{};
    constexpr char prefix[] = "/tmp/neotape-test-XXXXXX";
    std::copy(std::begin(prefix), std::end(prefix), pattern.begin());
    char *created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
        throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
}

TemporaryDirectory::~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

} // namespace neotape::test
