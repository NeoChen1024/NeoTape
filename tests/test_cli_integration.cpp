#include "support/checks.hpp"
#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using neotape::test::Process;
using neotape::test::ProcessOptions;
using neotape::test::ProcessResult;
using neotape::test::TemporaryDirectory;

ProcessResult run(const std::vector<std::string> &arguments,
                  std::chrono::milliseconds timeout = 30s) {
    return Process::run(ProcessOptions{arguments}, timeout);
}

using neotape::test::require_success;

} // namespace

TEST_CASE("CLI help exits successfully", "[integration][cli]") {
    for (const char *program :
         {NEOTAPE_MT_PAX, NEOTAPE_PLAN, NEOTAPE_ARCHIVER, NEOTAPE_RAW_STORE,
          NEOTAPE_WRITE, NEOTAPE_READ, NEOTAPE_EXTRACTOR, NEOTAPE_INSPECT,
          NEOTAPE_SCAN, NEOTAPE_DUMP}) {
        INFO(program);
        require_success(run({program, "--help"}));
    }
}

TEST_CASE("short CLI options create plan and pax output",
          "[integration][cli]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input";
    fs::path const plan = temporary.path() / "plan";
    fs::path const archive = temporary.path() / "output.pax";
    fs::create_directory(input);
    std::ofstream(input / "file.txt") << "short option test\n";

    require_success(run({NEOTAPE_PLAN, "-C", input.string(), "-s", "16M", "-m",
                         "4M", "-j", "1", "-o", plan.string(), "file.txt"}));
    require_success(run({NEOTAPE_MT_PAX, "-C", input.string(), "-B", "8M", "-j",
                         "1", "-f", archive.string(), "file.txt"}));

    REQUIRE(fs::file_size(plan) > 0);
    REQUIRE(fs::file_size(archive) > 0);
}
