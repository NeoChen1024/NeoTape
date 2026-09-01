#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using neotape::test::Process;
using neotape::test::ProcessOptions;
using neotape::test::ProcessResult;
using neotape::test::TemporaryDirectory;

void require_success(const ProcessResult &result) {
    INFO("stdout:\n" << result.standard_output);
    INFO("stderr:\n" << result.standard_error);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
}

TEST_CASE("mt-pax defers file opens under descriptor pressure",
          "[integration][pax][resource-limit]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "source";
    fs::path const fifo = temporary.path() / "archive.pipe";
    fs::create_directory(source);
    for (int index = 0; index < 60; ++index) {
        fs::path const file = source / ("file-" + std::to_string(index) + ".bin");
        { std::ofstream stream(file); }
        fs::resize_file(file, 5 * 1024 * 1024);
    }
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

    Process slow_reader(ProcessOptions{
        {NEOTAPE_DD, "if=" + fifo.string(), "of=/dev/null", "bs=64K"}});
    ProcessOptions writer_options{
        {NEOTAPE_MT_PAX, "-f", fifo.string(), "-C", temporary.path().string(),
         "--io-thread", "1", "--output-buffer-size", "1M", "source"}};
    writer_options.open_file_limit = 28;
    Process writer(std::move(writer_options));
    ProcessResult const writer_result = writer.wait(30s);
    require_success(writer_result);
    REQUIRE(writer_result.standard_error.find("Can't open") == std::string::npos);
    require_success(slow_reader.wait(10s));
}

} // namespace

TEST_CASE("multi-threaded mt-pax preserves files and symlinks",
          "[integration][pax]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "src";
    fs::path const directories = source / "dirs";
    fs::path const small = source / "small";
    fs::path const archive = temporary.path() / "archive.tar";
    fs::path const output = temporary.path() / "out";
    fs::create_directories(directories);
    fs::create_directories(small);
    fs::create_directory(output);

    for (int index = 0; index < 80; ++index) {
        fs::path const directory = directories / ("dir-" + std::to_string(index));
        fs::create_directory(directory);
        std::ofstream(small / ("file-" + std::to_string(index) + ".txt"))
            << "small file " << index << '\n';
        fs::create_symlink("../../small/file-" + std::to_string(index) + ".txt",
                           directory / ("link-" + std::to_string(index)));
    }
    std::ofstream(source / "large-a.bin");
    std::ofstream(source / "large-b.bin");
    fs::resize_file(source / "large-a.bin", 5 * 1024 * 1024);
    fs::resize_file(source / "large-b.bin", 6 * 1024 * 1024);

    ProcessResult const archive_result = Process::run(
        ProcessOptions{{NEOTAPE_MT_PAX, "-f", archive.string(), "-C",
                        temporary.path().string(), "--io-thread", "4", "-P",
                        "25", "--output-buffer-size", "8M", "src"}},
        120s);
    require_success(archive_result);
    REQUIRE(fs::file_size(archive) > 0);

    ProcessResult const extract_result = Process::run(
        ProcessOptions{{NEOTAPE_BSDTAR, "-xpf", archive.string(), "-C",
                        output.string()}},
        30s);
    require_success(extract_result);

    std::ifstream expected(small / "file-17.txt");
    std::ifstream actual(output / "src/small/file-17.txt");
    REQUIRE(std::string(std::istreambuf_iterator<char>(expected), {}) ==
            std::string(std::istreambuf_iterator<char>(actual), {}));
    REQUIRE(fs::is_symlink(output / "src/dirs/dir-17/link-17"));
}
