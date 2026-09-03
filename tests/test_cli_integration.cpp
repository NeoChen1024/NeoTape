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

void require_success(const ProcessResult &result) {
    INFO("stdout:\n" << result.standard_output);
    INFO("stderr:\n" << result.standard_error);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
}

void check_help(const std::string &program,
                const std::vector<std::string_view> &expected) {
    ProcessResult const result = run({program, "--help"});
    require_success(result);
    std::string const output = result.standard_output + result.standard_error;
    for (std::string_view text : expected) {
        INFO("expected help fragment: " << text);
        REQUIRE(output.find(text) != std::string::npos);
    }
}

} // namespace

TEST_CASE("CLI help exposes supported options", "[integration][cli]") {
    check_help(NEOTAPE_MT_PAX,
               {"-j|--io-thread", "-B|--output-buffer-size", "-p|--plan",
                "-S|--slice-output-prefix", "SIZE accepts K, M, G, or T"});
    check_help(NEOTAPE_PLAN,
               {"-s|--slice-size", "-m|--metadata-buffer-size",
                "-j|--io-threads", "SIZE accepts K, M, G, or T"});
    check_help(NEOTAPE_ARCHIVER,
               {"-l|--listen", "-B|--output-buffer-size",
                "-r|--retention-frame-count", "-F|--fec",
                "-k|--sign-secret-key", "SIZE accepts K, M, G, or T"});
    check_help(NEOTAPE_RAW_STORE,
               {"-l|--listen", "-b|--volume-block-size",
                "-r|--retention-frame-count", "-F|--fec",
                "SIZE accepts K, M, G, or T"});
    check_help(NEOTAPE_WRITE,
               {"-s|--source", "-t|--target", "-B|--output-buffer-size",
                "-m|--max-volume-bytes", "-R|--recovery-bundle",
                "-r|--recovery-bundle-block-size",
                "tape:/dev/nst0|spool:./dir|null",
                "SIZE accepts K, M, G, or T"});
    check_help(NEOTAPE_EXTRACTOR,
               {"-l|--listen", "-k|--verify-pubkey",
                "-S|--require-signed", "-s|--salvage"});
    check_help(NEOTAPE_INSPECT,
               {"-s|--source", "-k|--verify-pubkey",
                "-S|--require-signed", "-d|--debug", "-r|--raw"});
    check_help(NEOTAPE_READ, {"-s|--source", "-c|--connect"});
    check_help(NEOTAPE_SCAN, {"-s|--source", "-v|--verbose"});
    check_help(NEOTAPE_DUMP,
               {"-s|--source", "-t|--target", "-v|--verbose"});
}

TEST_CASE("short CLI options create plan and pax output",
          "[integration][cli]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input";
    fs::path const plan = temporary.path() / "plan";
    fs::path const archive = temporary.path() / "output.pax";
    fs::create_directory(input);
    std::ofstream(input / "file.txt") << "short option test\n";

    require_success(run({NEOTAPE_PLAN, "-C", input.string(), "-s", "16M",
                         "-m", "4M", "-j", "1", "-o", plan.string(),
                         "file.txt"}));
    require_success(run({NEOTAPE_MT_PAX, "-C", input.string(), "-B", "8M",
                         "-j", "1", "-f", archive.string(), "file.txt"}));

    REQUIRE(fs::file_size(plan) > 0);
    REQUIRE(fs::file_size(archive) > 0);
}
