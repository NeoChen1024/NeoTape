#include "neotape/format.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include "support/checks.hpp"
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
#include <sys/socket.h>
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

using neotape::test::read_bytes;

using neotape::test::write_pattern;

using neotape::test::require_contains;

} // namespace

TEST_CASE("archiver and writer create a valid spool",
          "[integration][archive][socket]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "source";
    fs::path const socket = temporary.path() / "archiver.sock";
    fs::path const spool = temporary.path() / "spool";
    fs::create_directory(source);
    std::ofstream(source / "a.txt") << "hello\n";

    ProcessResult const missing_listener =
        Process::run(ProcessOptions{{NEOTAPE_ARCHIVER, source.string()}}, 10s);
    REQUIRE(missing_listener.exit_code != 0);
    require_contains(missing_listener, "--listen is required");

    Process archiver(
        ProcessOptions{{NEOTAPE_ARCHIVER, "-l", "unix://" + socket.string(),
                        "-b", "4K", "-n", "smoke", source.string()}});
    REQUIRE(wait_for_unix_socket(socket, archiver, 5s));

    ProcessResult const writer = Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "-s", "unix://" + socket.string(), "-t",
                        "spool:" + spool.string(), "-B", "8M"}},
        30s);
    require_success(writer);
    ProcessResult const archiver_result = archiver.wait(30s);
    require_success(archiver_result);

    std::vector<fs::path> const files = spool_files(spool);
    REQUIRE(files.size() == 2);
    std::vector<std::byte> const content = read_bytes(files.front());
    std::vector<std::byte> const archive_end = read_bytes(files.back());
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
         "--volume-block-size", "4096", "--retention-frame-count", "1",
         "--archive-name", "null-test", "-C", source.string(), "."}});
    REQUIRE(wait_for_unix_socket(socket, archiver, 5s));

    int volume_count = 0;
    for (; volume_count < 32; ++volume_count) {
        ProcessResult const writer = Process::run(
            ProcessOptions{{NEOTAPE_WRITE, "--source",
                            "unix://" + socket.string(), "--target", "null",
                            "--max-volume-bytes", "4K"}},
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
    ProcessResult const archiver_result = archiver.wait(30s);
    require_success(archiver_result);
}

TEST_CASE("unacknowledged archive end is retained after a protocol error",
          "[integration][archive][socket]") {
    TemporaryDirectory temporary;
    fs::path const socket = temporary.path() / "raw.sock";
    Process server(
        ProcessOptions{{NEOTAPE_RAW_STORE, "-l", "unix://" + socket.string(),
                        "-b", "4K", "--input", "/dev/null"}});
    REQUIRE(wait_for_unix_socket(socket, server, 5s));

    std::vector<std::byte> archive_end;
    for (bool const acknowledge : {false, true}) {
        neotape::FdGuard client(
            neotape::connect_to_server("unix://" + socket.string()));
        timeval timeout{5, 0};
        REQUIRE(::setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                             sizeof(timeout)) == 0);
        using neotape::tcp::MessageType;
        neotape::tcp::write_message(client.fd, {MessageType::next_frame});
        auto response = neotape::tcp::read_message(client.fd);
        if (response.has_value() && response->type == MessageType::tape_eof) {
            neotape::tcp::write_message(client.fd, {MessageType::next_frame});
            response = neotape::tcp::read_message(client.fd);
        }
        REQUIRE(response.has_value());
        REQUIRE(response->type == MessageType::frame_record);
        auto const header = neotape::parse_fixed_header(
            reinterpret_cast<const uint8_t *>(response->payload.data()),
            response->payload.size());
        REQUIRE(header.channel_type == neotape::ChannelType::ARCHIVE_END);
        if (acknowledge) {
            REQUIRE(response->payload == archive_end);
            neotape::tcp::write_message(
                client.fd,
                {MessageType::ack_frame,
                 neotape::uint64_to_le_bytes(header.global_frame_seq_num)});
        } else {
            archive_end = response->payload;
            neotape::tcp::write_message(client.fd, {MessageType::next_frame});
            response = neotape::tcp::read_message(client.fd);
            REQUIRE(response.has_value());
            REQUIRE(response->type == MessageType::error);
            REQUIRE_FALSE(neotape::tcp::read_message(client.fd).has_value());
        }
    }
    require_success(server.wait(5s));
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
        ProcessOptions{{NEOTAPE_WRITE, "--source", "unix://" + socket.string(),
                        "--target", "spool:" + spool.string(), "--erase",
                        "--recovery-bundle", bundle.string()}},
        30s);
    require_success(writer);
    require_success(raw_store.wait(30s));
    REQUIRE(read_bytes(bundle) == read_bytes(spool / "recovery-bundle.tar"));

    std::vector<fs::path> const files = spool_files(spool);
    REQUIRE(files.size() == 2);
    REQUIRE(read_bytes(files.front()).size() == 4 * block_size);
    REQUIRE(read_bytes(files.back()).size() == block_size);

    ProcessResult const inspect =
        Process::run(ProcessOptions{{NEOTAPE_INSPECT, "--source",
                                     "spool:" + spool.string()}},
                     30s);
    require_success(inspect);
    require_contains(inspect, "Compliance: PASS");
    REQUIRE((inspect.standard_output + inspect.standard_error).find("| FAIL") ==
            std::string::npos);

    ProcessResult const scan = Process::run(
        ProcessOptions{{NEOTAPE_SCAN, "--source", "spool:" + spool.string()}},
        30s);
    require_success(scan);
    auto first_record = read_bytes(files.front());
    auto header = neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(first_record.data()),
        first_record.size());
    REQUIRE(scan.standard_output.find(header.archive_uuid) !=
            std::string::npos);

    ProcessResult const verbose_scan =
        Process::run(ProcessOptions{{NEOTAPE_SCAN, "--source",
                                     "spool:" + spool.string(), "--verbose"}},
                     30s);
    require_success(verbose_scan);
}
