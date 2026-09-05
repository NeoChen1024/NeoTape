#include "support/checks.hpp"
#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
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

using neotape::test::require_success;

using neotape::test::read_file;

void extract_pax(const fs::path &archive, const fs::path &destination) {
    fs::create_directory(destination);
    require_success(
        Process::run(ProcessOptions{{NEOTAPE_BSDTAR, "-xpf", archive.string(),
                                     "-C", destination.string()}},
                     30s));
}

using neotape::test::write_pattern;

void write_spool_from_archiver(const fs::path &socket, const fs::path &spool,
                               const fs::path &input) {
    Process archiver(ProcessOptions{{NEOTAPE_ARCHIVER, "--listen",
                                     "unix://" + socket.string(),
                                     "--archive-name", "restore-test", "-C",
                                     input.string(), "hello.txt", "bar.txt"}});
    REQUIRE(wait_for_unix_socket(socket, archiver, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source", "unix://" + socket.string(),
                        "--target", "spool:" + spool.string(), "--erase"}},
        30s));
    require_success(archiver.wait(30s));
}

void restore_spool(const fs::path &socket, const fs::path &spool,
                   const fs::path &output) {
    Process extractor(
        ProcessOptions{{NEOTAPE_EXTRACTOR, "--listen",
                        "unix://" + socket.string(), "-o", output.string()}});
    REQUIRE(wait_for_unix_socket(socket, extractor, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_READ, "--source", "spool:" + spool.string(),
                        "--connect", "unix://" + socket.string()}},
        30s));
    require_success(extractor.wait(30s));
}

} // namespace

TEST_CASE("archiver spool restores the original pax contents",
          "[integration][restore][socket]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input";
    fs::path const extracted = temporary.path() / "extracted.pax";
    fs::path const spool = temporary.path() / "spool";
    fs::create_directory(input);
    std::ofstream(input / "hello.txt") << "hello world\n";
    std::ofstream(input / "bar.txt") << "another file\n";

    write_spool_from_archiver(temporary.path() / "archiver.sock", spool, input);
    restore_spool(temporary.path() / "extractor.sock", spool, extracted);

    fs::path const extracted_output = temporary.path() / "extracted-output";
    extract_pax(extracted, extracted_output);
    REQUIRE(read_file(input / "hello.txt") ==
            read_file(extracted_output / "hello.txt"));
    REQUIRE(read_file(input / "bar.txt") ==
            read_file(extracted_output / "bar.txt"));
}

TEST_CASE("planner preserves hardlinks in generated pax slices",
          "[integration][plan][hardlink]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "source";
    fs::path const plan = temporary.path() / "plan";
    fs::path const prefix = temporary.path() / "planned-";
    fs::create_directory(source);
    std::ofstream(source / "a.txt") << "hello hardlink\n";
    fs::create_hard_link(source / "a.txt", source / "b.txt");

    require_success(
        Process::run(ProcessOptions{{NEOTAPE_PLAN, "-C", source.string(), "-o",
                                     plan.string(), "a.txt", "b.txt"}},
                     30s));

    require_success(
        Process::run(ProcessOptions{{NEOTAPE_MT_PAX, "--plan", plan.string(),
                                     "--slice-output-prefix", prefix.string(),
                                     "--io-thread", "4"}},
                     60s));
    fs::path const output = temporary.path() / "output";
    extract_pax(temporary.path() / "planned-000000.pax", output);
    REQUIRE(read_file(source / "a.txt") == read_file(output / "a.txt"));
    REQUIRE(read_file(source / "b.txt") == read_file(output / "b.txt"));
    REQUIRE(fs::equivalent(output / "a.txt", output / "b.txt"));
}

TEST_CASE("two volumes restore one continuous archive",
          "[integration][restore][multi-volume][socket]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "source";
    fs::path const restored = temporary.path() / "restored.pax";
    fs::path const spool_one = temporary.path() / "volume-one";
    fs::path const spool_two = temporary.path() / "volume-two";
    fs::path const archiver_socket = temporary.path() / "archiver.sock";
    fs::path const extractor_socket = temporary.path() / "extractor.sock";
    fs::create_directory(source);
    write_pattern(source / "blob.bin", 5 * 1024 * 1024);

    Process archiver(ProcessOptions{
        {NEOTAPE_ARCHIVER, "--listen", "unix://" + archiver_socket.string(),
         "--volume-block-size", "4096", "--archive-name", "multi-restore",
         "--retention-frame-count", "1", "-C", source.string(), "blob.bin"}});
    REQUIRE(wait_for_unix_socket(archiver_socket, archiver, 5s));

    ProcessResult const first_writer = Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source",
                        "unix://" + archiver_socket.string(), "--target",
                        "spool:" + spool_one.string(), "--output-buffer-size",
                        "8388608", "--max-volume-bytes", "32768"}},
        30s);
    INFO(first_writer.standard_error);
    REQUIRE_FALSE(first_writer.timed_out);
    REQUIRE(first_writer.exit_code == 3);
    require_success(
        Process::run(ProcessOptions{{NEOTAPE_WRITE, "--source",
                                     "unix://" + archiver_socket.string(),
                                     "--target", "spool:" + spool_two.string(),
                                     "--output-buffer-size", "8388608"}},
                     60s));
    require_success(archiver.wait(60s));

    ProcessResult const first_inspect =
        Process::run(ProcessOptions{{NEOTAPE_INSPECT, "--source",
                                     "spool:" + spool_one.string()}},
                     30s);
    require_success(first_inspect);
    REQUIRE((first_inspect.standard_output + first_inspect.standard_error)
                .find("Compliance: PASS") != std::string::npos);
    REQUIRE((first_inspect.standard_output + first_inspect.standard_error)
                .find("Archive_end:      0") != std::string::npos);
    ProcessResult const second_inspect =
        Process::run(ProcessOptions{{NEOTAPE_INSPECT, "--source",
                                     "spool:" + spool_two.string()}},
                     30s);
    require_success(second_inspect);
    REQUIRE((second_inspect.standard_output + second_inspect.standard_error)
                .find("Compliance: PASS") != std::string::npos);

    Process extractor(ProcessOptions{{NEOTAPE_EXTRACTOR, "--listen",
                                      "unix://" + extractor_socket.string(),
                                      "-o", restored.string()}});
    REQUIRE(wait_for_unix_socket(extractor_socket, extractor, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_READ, "--source", "spool:" + spool_one.string(),
                        "--connect", "unix://" + extractor_socket.string()}},
        30s));
    REQUIRE(extractor.running());
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_READ, "--source", "spool:" + spool_two.string(),
                        "--connect", "unix://" + extractor_socket.string()}},
        60s));
    require_success(extractor.wait(60s));

    fs::path const restored_output = temporary.path() / "restored-output";
    extract_pax(restored, restored_output);
    REQUIRE(fs::file_size(source / "blob.bin") == 5 * 1024 * 1024);
    REQUIRE(read_file(source / "blob.bin") ==
            read_file(restored_output / "blob.bin"));
}

TEST_CASE("plan-driven archiver restores the planned slice",
          "[integration][plan][restore][socket]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input";
    fs::path const plan = temporary.path() / "plan";
    fs::path const spool = temporary.path() / "spool";
    fs::path const restored = temporary.path() / "restored.pax";
    fs::path const archiver_socket = temporary.path() / "archiver.sock";
    fs::path const extractor_socket = temporary.path() / "extractor.sock";
    fs::create_directory(input);
    std::ofstream(input / "hello.txt") << "hello plan mode\n";
    write_pattern(input / "blob.bin", 6 * 1024 * 1024);

    require_success(
        Process::run(ProcessOptions{{NEOTAPE_PLAN, "-C", input.string(), "-o",
                                     plan.string(), "hello.txt", "blob.bin"}},
                     60s));

    Process archiver(ProcessOptions{
        {NEOTAPE_ARCHIVER, "--listen", "unix://" + archiver_socket.string(),
         "--archive-name", "plan-restore", "--io-thread", "4", "--plan",
         plan.string(), "--output-buffer-size", "1M", "-P", "80"}});
    REQUIRE(wait_for_unix_socket(archiver_socket, archiver, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source",
                        "unix://" + archiver_socket.string(), "--target",
                        "spool:" + spool.string(), "--erase"}},
        60s));
    require_success(archiver.wait(60s));
    restore_spool(extractor_socket, spool, restored);

    fs::path const restored_output = temporary.path() / "restored-output";
    extract_pax(restored, restored_output);
    REQUIRE(read_file(input / "hello.txt") ==
            read_file(restored_output / "hello.txt"));
    REQUIRE(read_file(input / "blob.bin") ==
            read_file(restored_output / "blob.bin"));
}
