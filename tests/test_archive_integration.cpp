#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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
using neotape::test::wait_for_unix_socket;

void require_success(const ProcessResult &result) {
    INFO("stdout:\n" << result.standard_output);
    INFO("stderr:\n" << result.standard_error);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
}

std::vector<fs::path> spool_files(const fs::path &directory) {
    std::vector<fs::path> files;
    for (const fs::directory_entry &entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".nts") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

std::vector<std::byte> read_binary(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<char> characters{std::istreambuf_iterator<char>(input), {}};
    std::vector<std::byte> bytes;
    bytes.reserve(characters.size());
    std::ranges::transform(characters, std::back_inserter(bytes),
                           [](char value) {
                               return static_cast<std::byte>(
                                   static_cast<unsigned char>(value));
                           });
    return bytes;
}

void write_pattern(const fs::path &path, std::size_t size) {
    std::ofstream output(path, std::ios::binary);
    for (std::size_t index = 0; index < size; ++index) {
        output.put(static_cast<char>(index % 251));
    }
}

void require_contains(const ProcessResult &result, std::string_view text) {
    std::string const output = result.standard_output + result.standard_error;
    INFO("output:\n" << output);
    REQUIRE(output.find(text) != std::string::npos);
}

} // namespace

TEST_CASE("archiver and writer create a valid spool",
          "[integration][archive][socket]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "source";
    fs::path const socket = temporary.path() / "archiver.sock";
    fs::path const spool = temporary.path() / "spool";
    fs::create_directory(source);
    std::ofstream(source / "a.txt") << "hello\n";

    ProcessResult const missing_listener = Process::run(
        ProcessOptions{{NEOTAPE_ARCHIVER, source.string()}}, 10s);
    REQUIRE(missing_listener.exit_code != 0);
    require_contains(missing_listener, "--listen is required");

    Process archiver(ProcessOptions{
        {NEOTAPE_ARCHIVER, "-l", "unix://" + socket.string(), "-b", "4K",
         "-n", "smoke", source.string()}});
    REQUIRE(wait_for_unix_socket(socket, archiver, 5s));

    ProcessResult const writer = Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "-s", "unix://" + socket.string(),
                        "-t", "spool:" + spool.string(), "-B", "8M"}},
        30s);
    require_success(writer);
    require_contains(writer, "block_size=4096");
    require_contains(writer, "archive_label=\"smoke\"");
    require_contains(writer, "volume_seq=1");
    require_contains(writer, "slice_seq=0");
    require_success(archiver.wait(30s));

    std::vector<fs::path> const files = spool_files(spool);
    REQUIRE(files.size() == 2);
    std::vector<std::byte> const content = read_binary(files.front());
    std::vector<std::byte> const archive_end = read_binary(files.back());
    REQUIRE(content.size() >= 4096);
    REQUIRE(archive_end.size() == 4096);
    REQUIRE(std::to_integer<unsigned char>(content[9]) == 1);
    REQUIRE(std::to_integer<unsigned char>(archive_end[9]) == 0xff);
    REQUIRE(std::string(reinterpret_cast<const char *>(content.data()), 8) ==
            std::string("NeoTape\0", 8));
}

TEST_CASE("null writer validates an archive across capacity-limited volumes",
          "[integration][archive][null][socket]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "source";
    fs::path const socket = temporary.path() / "archiver.sock";
    fs::create_directory(source);
    write_pattern(source / fs::path("opaque-\x82\xe7.bin"), 64 * 1024);

    Process archiver(ProcessOptions{
        {NEOTAPE_ARCHIVER, "--listen", "unix://" + socket.string(),
         "--volume-block-size", "4096", "--archive-name", "null-test",
         "-C", source.string(), "."}});
    REQUIRE(wait_for_unix_socket(socket, archiver, 5s));

    int volume_count = 0;
    for (; volume_count < 32; ++volume_count) {
        ProcessResult const writer = Process::run(
            ProcessOptions{{NEOTAPE_WRITE, "--source",
                            "unix://" + socket.string(), "--target", "null",
                            "--max-volume-bytes", "16K"}},
            30s);
        INFO("stdout:\n" << writer.standard_output);
        INFO("stderr:\n" << writer.standard_error);
        REQUIRE_FALSE(writer.timed_out);
        REQUIRE((writer.exit_code == 0 || writer.exit_code == 3));
        if (writer.exit_code == 0) {
            break;
        }
    }

    REQUIRE(volume_count > 0);
    REQUIRE(volume_count < 32);
    require_success(archiver.wait(30s));
}

TEST_CASE("raw spool passes inspect and scan reporting",
          "[integration][raw-store][inspect][scan][socket]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input.bin";
    fs::path const socket = temporary.path() / "raw.sock";
    fs::path const spool = temporary.path() / "spool";
    fs::path const bundle = temporary.path() / "recovery.tar";
    constexpr std::size_t block_size = 4096;
    constexpr std::size_t capacity = block_size - 512;
    write_pattern(input, capacity * 3 + 100);

    require_success(Process::run(
        ProcessOptions{{NEOTAPE_BSDTAR, "-cf", bundle.string(), "-C",
                        temporary.path().string(), "input.bin"}},
        30s));
    fs::create_directory(spool);
    std::ofstream(spool / "recovery-bundle.tar") << "stale bundle\n";
    ProcessResult const conflicting_options = Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source", "unix:///nonexistent",
                        "--target", "spool:" + spool.string(), "--append",
                        "--recovery-bundle", bundle.string()}},
        10s);
    REQUIRE(conflicting_options.exit_code != 0);
    require_contains(conflicting_options,
                     "--recovery-bundle cannot be used with --append");

    Process raw_store(ProcessOptions{
        {NEOTAPE_RAW_STORE, "--listen", "unix://" + socket.string(),
         "--volume-block-size", "4096", "--archive-name", "inspect-test",
         "--retention-frame-count", "1", "--input", input.string()}});
    REQUIRE(wait_for_unix_socket(socket, raw_store, 5s));

    ProcessResult const writer = Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source",
                        "unix://" + socket.string(), "--target",
                        "spool:" + spool.string(), "--erase",
                        "--recovery-bundle", bundle.string()}},
        30s);
    require_success(writer);
    require_success(raw_store.wait(30s));
    REQUIRE(read_binary(bundle) == read_binary(spool / "recovery-bundle.tar"));

    std::vector<fs::path> const files = spool_files(spool);
    REQUIRE(files.size() == 2);
    REQUIRE(read_binary(files.front()).size() == 4 * block_size);
    REQUIRE(read_binary(files.back()).size() == block_size);

    ProcessResult const inspect = Process::run(
        ProcessOptions{{NEOTAPE_INSPECT, "--source",
                        "spool:" + spool.string()}},
        30s);
    require_success(inspect);
    require_contains(inspect, "Compliance: PASS");
    require_contains(inspect, "Total frames:     5");
    require_contains(inspect, "Content frames:   4");
    require_contains(inspect, "Archive_end:      1");
    require_contains(inspect, "Archive label:    \"inspect-test\"");
    require_contains(inspect, "Volume block:     4096 bytes");
    REQUIRE((inspect.standard_output + inspect.standard_error).find("| FAIL") ==
            std::string::npos);

    ProcessResult const scan = Process::run(
        ProcessOptions{{NEOTAPE_SCAN, "--source", "spool:" + spool.string()}},
        30s);
    require_success(scan);
    require_contains(scan, "Archive first seen at tapefile #0: archive_uuid=");
    require_contains(scan, "Unique archives found: 1");
    require_contains(scan, "archive_label=\"inspect-test\"");
    require_contains(scan, "Tapefiles scanned: 2");

    ProcessResult const verbose_scan = Process::run(
        ProcessOptions{{NEOTAPE_SCAN, "--source", "spool:" + spool.string(),
                        "--verbose"}},
        30s);
    require_success(verbose_scan);
    require_contains(verbose_scan,
                     "Tapefile #0: channel=CH_CONTENT global_frame_seq_num=0");
    require_contains(verbose_scan,
                     "Tapefile #1: channel=ARCHIVE_END global_frame_seq_num=4");
}
