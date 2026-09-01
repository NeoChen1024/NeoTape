#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

void write_pattern(const fs::path &path, std::size_t size) {
    std::ofstream output(path, std::ios::binary);
    for (std::size_t index = 0; index < size; ++index) {
        output.put(static_cast<char>((index * 29 + 7) % 251));
    }
}

std::vector<char> read_binary(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

fs::path content_file(const fs::path &spool) {
    for (const fs::directory_entry &entry : fs::directory_iterator(spool)) {
        if (entry.path().filename().string().find(".slice-") !=
            std::string::npos) {
            return entry.path();
        }
    }
    throw std::runtime_error("content spool file not found");
}

void flip_payload_byte(const fs::path &path, std::size_t record_index) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    std::streamoff const offset =
        static_cast<std::streamoff>(record_index * 4096 + 512);
    stream.seekg(offset);
    char byte = 0;
    stream.read(&byte, 1);
    byte ^= 1;
    stream.seekp(offset);
    stream.write(&byte, 1);
}

void create_raw_spool(const fs::path &input, const fs::path &socket,
                      const fs::path &spool, bool fec) {
    std::vector<std::string> arguments{
        NEOTAPE_RAW_STORE, "--listen", "unix://" + socket.string(),
        "--input", input.string(), "--volume-block-size", "4096",
        "--archive-name", "recovery-test"};
    if (fec) {
        arguments.emplace_back("--fec");
    }
    Process raw_store(ProcessOptions{std::move(arguments)});
    REQUIRE(wait_for_unix_socket(socket, raw_store, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source", "unix://" + socket.string(),
                        "--target", "spool:" + spool.string()}},
        60s));
    require_success(raw_store.wait(60s));
}

ProcessResult extract(const fs::path &socket, const fs::path &spool,
                      const fs::path &output, bool salvage = false,
                      bool require_reader_success = true) {
    std::vector<std::string> arguments{NEOTAPE_EXTRACTOR, "--listen",
                                       "unix://" + socket.string(), "-o",
                                       output.string()};
    if (salvage) {
        arguments.emplace_back("--salvage");
    }
    Process extractor(ProcessOptions{std::move(arguments)});
    REQUIRE(wait_for_unix_socket(socket, extractor, 5s));
    ProcessResult const reader = Process::run(
        ProcessOptions{{NEOTAPE_READ, "--source", "spool:" + spool.string(),
                        "--connect", "unix://" + socket.string()}},
        60s);
    if (require_reader_success) {
        require_success(reader);
    }
    return extractor.wait(60s);
}

} // namespace

TEST_CASE("FEC restores a corrupt protected content shard",
          "[integration][fec][recovery][socket]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input.bin";
    fs::path const spool = temporary.path() / "spool";
    constexpr std::size_t capacity = 4096 - 512;
    write_pattern(input, capacity * 33 + 123);
    create_raw_spool(input, temporary.path() / "raw.sock", spool, true);

    ProcessResult const inspect = Process::run(
        ProcessOptions{{NEOTAPE_INSPECT, "--source",
                        "spool:" + spool.string()}},
        30s);
    require_success(inspect);
    REQUIRE((inspect.standard_output + inspect.standard_error)
                .find("Compliance: PASS") != std::string::npos);
    REQUIRE((inspect.standard_output + inspect.standard_error).find("fec") !=
            std::string::npos);

    fs::path const first_output = temporary.path() / "first.bin";
    require_success(extract(temporary.path() / "extract.sock", spool,
                            first_output));
    REQUIRE(read_binary(input) == read_binary(first_output));

    flip_payload_byte(content_file(spool), 1);
    fs::path const repaired = temporary.path() / "repaired.bin";
    ProcessResult const recovery = extract(temporary.path() / "repair.sock",
                                           spool, repaired);
    require_success(recovery);
    REQUIRE(read_binary(input) == read_binary(repaired));
    REQUIRE(recovery.standard_error.find(
                "FEC repaired 1 unavailable content shard(s)") !=
            std::string::npos);

    flip_payload_byte(content_file(spool), 32);
    fs::path const repaired_twice = temporary.path() / "repaired-twice.bin";
    ProcessResult const two_erasure_recovery = extract(
        temporary.path() / "repair-two.sock", spool, repaired_twice);
    require_success(two_erasure_recovery);
    REQUIRE(read_binary(input) == read_binary(repaired_twice));
    REQUIRE(two_erasure_recovery.standard_error.find("FEC repair unavailable") !=
            std::string::npos);

    for (std::size_t record_index : {2U, 3U, 4U}) {
        flip_payload_byte(content_file(spool), record_index);
    }
    ProcessResult const unrecoverable =
        extract(temporary.path() / "unrecoverable.sock", spool,
                temporary.path() / "unrecoverable.bin", false, false);
    REQUIRE_FALSE(unrecoverable.timed_out);
    REQUIRE(unrecoverable.exit_code != 0);
    REQUIRE(unrecoverable.standard_error.find("FEC recovery failed") !=
            std::string::npos);
    REQUIRE(unrecoverable.standard_error.find(
                "unrecoverable frame validation failure") !=
            std::string::npos);
}

TEST_CASE("salvage skips a corrupt unprotected frame with warnings",
          "[integration][salvage][socket]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input.bin";
    fs::path const spool = temporary.path() / "spool";
    fs::path const output = temporary.path() / "output.bin";
    constexpr std::size_t capacity = 4096 - 512;
    write_pattern(input, capacity * 2 + 123);
    create_raw_spool(input, temporary.path() / "raw.sock", spool, false);
    flip_payload_byte(content_file(spool), 1);

    ProcessResult const result =
        extract(temporary.path() / "extract.sock", spool, output, true);
    require_success(result);
    std::vector<char> const original = read_binary(input);
    std::vector<char> expected;
    expected.insert(expected.end(), original.begin(), original.begin() + capacity);
    expected.insert(expected.end(), original.begin() + 2 * capacity,
                    original.end());
    REQUIRE(expected == read_binary(output));
    REQUIRE(result.standard_error.find(
                "SALVAGE MODE: output is not fully verified") !=
            std::string::npos);
    REQUIRE(result.standard_error.find(
                "salvage skipped frame: frame hash mismatch") !=
            std::string::npos);
}
