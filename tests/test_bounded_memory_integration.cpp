#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using neotape::test::Process;
using neotape::test::ProcessOptions;
using neotape::test::ProcessResult;
using neotape::test::TemporaryDirectory;
using neotape::test::wait_for_unix_socket;

void require_success(const ProcessResult &result) {
    INFO("stdout:\n" << result.standard_output);
    INFO("stderr:\n" << result.standard_error);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
}

} // namespace

TEST_CASE("FEC extraction stays below a slice-sized address-space limit",
          "[integration][fec][bounded-memory][large][socket]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input.bin";
    fs::path const output = temporary.path() / "output.bin";
    fs::path const spool = temporary.path() / "spool";
    fs::path const raw_socket = temporary.path() / "raw.sock";
    fs::path const extract_socket = temporary.path() / "extract.sock";
    { std::ofstream stream(input); }
    fs::resize_file(input, 320ULL * 1024 * 1024);

    Process raw_store(ProcessOptions{
        {NEOTAPE_RAW_STORE, "--listen", "unix://" + raw_socket.string(),
         "--input", input.string(), "--volume-block-size", "1048576",
         "--archive-name", "bounded-memory-test", "--fec"}});
    REQUIRE(wait_for_unix_socket(raw_socket, raw_store, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source", "unix://" + raw_socket.string(),
                        "--target", "spool:" + spool.string()}},
        180s));
    require_success(raw_store.wait(180s));

    ProcessOptions extractor_options{
        {NEOTAPE_EXTRACTOR, "--listen", "unix://" + extract_socket.string(),
         "-o", output.string()}};
    extractor_options.address_space_limit = 256ULL * 1024 * 1024;
    Process extractor(std::move(extractor_options));
    REQUIRE(wait_for_unix_socket(extract_socket, extractor, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_READ, "--source", "spool:" + spool.string(),
                        "--connect", "unix://" + extract_socket.string()}},
        180s));
    ProcessResult const extraction = extractor.wait(180s);
    require_success(extraction);
    REQUIRE(extraction.standard_error.find("archive extraction complete") !=
            std::string::npos);
    REQUIRE(fs::file_size(output) == fs::file_size(input));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_CMP, input.string(), output.string()}}, 180s));
}
