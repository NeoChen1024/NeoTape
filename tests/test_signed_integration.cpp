#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include "support/checks.hpp"
#include "support/process.hpp"
#include "support/temp_directory.hpp"
#include <poll.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using neotape::test::Process;
using neotape::test::ProcessOptions;
using neotape::test::ProcessResult;
using neotape::test::TemporaryDirectory;
using neotape::test::wait_for_unix_socket;

using neotape::test::require_success;

using neotape::test::output;
using neotape::test::require_contains;

using neotape::test::read_file;

using neotape::test::content_file;

int serve_corrupt_record(const fs::path &socket_path, std::vector<char> record,
                         std::promise<void> ready) {
    try {
        neotape::FdGuard server(
            neotape::create_listener("unix://" + socket_path.string()));
        ready.set_value();
        pollfd event{server.fd, POLLIN, 0};
        if (::poll(&event, 1, 5000) != 1)
            return 1;
        neotape::FdGuard client(::accept(server.fd, nullptr, nullptr));
        if (client.fd < 0)
            return 1;
        timeval timeout{5, 0};
        if (::setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)) ||
            ::setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout)))
            return 1;
        auto request = neotape::tcp::read_message(client.fd);
        if (!request || request->type != neotape::tcp::MessageType::next_frame)
            return 1;
        std::vector<std::byte> payload;
        payload.reserve(record.size());
        for (unsigned char byte : record)
            payload.push_back(static_cast<std::byte>(byte));
        neotape::tcp::write_message(
            client.fd,
            {neotape::tcp::MessageType::frame_record, std::move(payload)});
        return 0;
    } catch (...) {
        return 1;
    }
}

} // namespace

TEST_CASE("signed archive is authenticated and restored",
          "[integration][signature][authentication][socket]") {
    TemporaryDirectory temporary;
    fs::path const public_key = fs::path(NEOTAPE_SOURCE_DIR) /
                                "3rdparty/signify/regress/regresskey.pub";
    fs::path const secret_key = fs::path(NEOTAPE_SOURCE_DIR) /
                                "3rdparty/signify/regress/regresskey.sec";
    fs::path const input = temporary.path() / "input.bin";
    fs::path const output_file = temporary.path() / "output.bin";
    fs::path const spool = temporary.path() / "spool";
    fs::path const archiver_socket = temporary.path() / "archiver.sock";
    fs::path const extractor_socket = temporary.path() / "extractor.sock";
    std::ofstream(input) << "hello signed world\nanother signed line\n";

    Process archiver(ProcessOptions{
        {NEOTAPE_RAW_STORE, "--listen", "unix://" + archiver_socket.string(),
         "--archive-name", "signed-test", "--input", input.string(),
         "--sign-secret-key", secret_key.string()}});
    REQUIRE(wait_for_unix_socket(archiver_socket, archiver, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source",
                        "unix://" + archiver_socket.string(), "--target",
                        "spool:" + spool.string(), "--erase", "--verify-pubkey",
                        public_key.string()}},
        30s));
    require_success(archiver.wait(30s));

    ProcessResult const trusted_inspect = Process::run(
        ProcessOptions{{NEOTAPE_INSPECT, "--source", "spool:" + spool.string(),
                        "--verify-pubkey", public_key.string(),
                        "--require-signed"}},
        30s);
    require_success(trusted_inspect);
    require_contains(trusted_inspect, "Compliance: PASS");
    require_contains(trusted_inspect, "Signature errors: 0");
    REQUIRE(output(trusted_inspect).find("Signatures valid: 0") ==
            std::string::npos);

    ProcessResult const unverified_inspect =
        Process::run(ProcessOptions{{NEOTAPE_INSPECT, "--source",
                                     "spool:" + spool.string()}},
                     30s);
    require_success(unverified_inspect);
    require_contains(unverified_inspect, "Compliance: PASS");
    require_contains(unverified_inspect, "Signatures valid: 0");
    REQUIRE(output(unverified_inspect).find("Signed unverified: 0") ==
            std::string::npos);

    ProcessResult const inspect_without_key = Process::run(
        ProcessOptions{{NEOTAPE_INSPECT, "--source", "spool:" + spool.string(),
                        "--require-signed"}},
        10s);
    REQUIRE(inspect_without_key.exit_code == 2);
    require_contains(inspect_without_key,
                     "--require-signed requires at least one --verify-pubkey");

    ProcessResult const extractor_without_key = Process::run(
        ProcessOptions{
            {NEOTAPE_EXTRACTOR, "--listen",
             "unix://" + (temporary.path() / "invalid.sock").string(),
             "--require-signed"}},
        10s);
    REQUIRE(extractor_without_key.exit_code == 2);
    require_contains(extractor_without_key,
                     "--require-signed requires at least one --verify-pubkey");

    Process extractor(ProcessOptions{
        {NEOTAPE_EXTRACTOR, "--listen", "unix://" + extractor_socket.string(),
         "-o", output_file.string(), "--verify-pubkey", public_key.string(),
         "--require-signed"}});
    REQUIRE(wait_for_unix_socket(extractor_socket, extractor, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_READ, "--source", "spool:" + spool.string(),
                        "--connect", "unix://" + extractor_socket.string()}},
        30s));
    require_success(extractor.wait(30s));
    REQUIRE(read_file(input) == read_file(output_file));

    fs::path const unverified_socket = temporary.path() / "unverified.sock";
    fs::path const unverified_output = temporary.path() / "unverified.bin";
    Process unverified_extractor(ProcessOptions{
        {NEOTAPE_EXTRACTOR, "--listen", "unix://" + unverified_socket.string(),
         "-o", unverified_output.string()}});
    REQUIRE(wait_for_unix_socket(unverified_socket, unverified_extractor, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_READ, "--source", "spool:" + spool.string(),
                        "--connect", "unix://" + unverified_socket.string()}},
        30s));
    ProcessResult const unverified_result = unverified_extractor.wait(30s);
    require_success(unverified_result);
    require_contains(unverified_result, "signed frames are not authenticated");
    REQUIRE(read_file(output_file) == read_file(unverified_output));

    std::string record_text = read_file(content_file(spool));
    REQUIRE(record_text.size() >= 512);
    std::size_t const record_size =
        (static_cast<unsigned char>(record_text[10]) |
         (static_cast<std::size_t>(static_cast<unsigned char>(record_text[11]))
          << 8)) *
        1024;
    REQUIRE(record_text.size() >= record_size);
    std::vector<char> corrupt_record(
        record_text.begin(),
        record_text.begin() + static_cast<std::ptrdiff_t>(record_size));
    corrupt_record[512] ^= 1;
    fs::path const corrupt_socket = temporary.path() / "corrupt.sock";
    std::promise<void> ready;
    std::future<void> ready_future = ready.get_future();
    std::future<int> server_result =
        std::async(std::launch::async, serve_corrupt_record, corrupt_socket,
                   std::move(corrupt_record), std::move(ready));
    REQUIRE(ready_future.wait_for(5s) == std::future_status::ready);
    ProcessResult const corrupt_writer = Process::run(
        ProcessOptions{
            {NEOTAPE_WRITE, "--source", "unix://" + corrupt_socket.string(),
             "--target",
             "spool:" + (temporary.path() / "corrupt-spool").string(),
             "--erase"}},
        30s);
    REQUIRE_FALSE(corrupt_writer.timed_out);
    REQUIRE(corrupt_writer.exit_code != 0);
    require_contains(corrupt_writer, "frame hash mismatch");
    REQUIRE(server_result.get() == 0);
}

TEST_CASE("writer authentication rejects an unsigned source",
          "[integration][authentication][socket]") {
    TemporaryDirectory temporary;
    fs::path const public_key = fs::path(NEOTAPE_SOURCE_DIR) /
                                "3rdparty/signify/regress/regresskey.pub";
    fs::path const input = temporary.path() / "input.bin";
    fs::path const socket = temporary.path() / "unsigned.sock";
    fs::path const spool = temporary.path() / "spool";
    std::ofstream(input) << "unsigned authentication failure\n";

    Process raw_store(ProcessOptions{
        {NEOTAPE_RAW_STORE, "--listen", "unix://" + socket.string(), "--input",
         input.string(), "--archive-name", "unsigned-test"}});
    REQUIRE(wait_for_unix_socket(socket, raw_store, 5s));
    ProcessResult const writer = Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source", "unix://" + socket.string(),
                        "--target", "spool:" + spool.string(), "--erase",
                        "--verify-pubkey", public_key.string()}},
        30s);
    REQUIRE_FALSE(writer.timed_out);
    REQUIRE(writer.exit_code != 0);
    std::string const diagnostic = output(writer);
    INFO(diagnostic);
    REQUIRE((diagnostic.find("auth") != std::string::npos ||
             diagnostic.find("signer") != std::string::npos));
}
