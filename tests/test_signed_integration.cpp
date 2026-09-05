#include "support/checks.hpp"
#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
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

std::string output(const ProcessResult &result) {
    return result.standard_output + result.standard_error;
}

void require_contains(const ProcessResult &result, std::string_view text) {
    INFO("output:\n" << output(result));
    REQUIRE(output(result).find(text) != std::string::npos);
}

using neotape::test::read_file;

fs::path first_content_file(const fs::path &spool) {
    for (const fs::directory_entry &entry : fs::directory_iterator(spool)) {
        if (entry.path().filename().string().find(".slice-") !=
            std::string::npos) {
            return entry.path();
        }
    }
    throw std::runtime_error("signed content spool file not found");
}

bool transfer_exact(int fd, void *buffer, std::size_t size, bool send_data) {
    auto *bytes = static_cast<char *>(buffer);
    while (size > 0) {
        ssize_t const count = send_data ? ::send(fd, bytes, size, MSG_NOSIGNAL)
                                        : ::recv(fd, bytes, size, 0);
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

int serve_corrupt_record(const fs::path &socket_path, std::vector<char> record,
                         std::promise<void> ready) {
    int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        return 1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.string().size() >= sizeof(address.sun_path)) {
        ::close(server);
        return 2;
    }
    std::strcpy(address.sun_path, socket_path.c_str());
    if (::bind(server, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(server, 1) != 0) {
        ::close(server);
        return 3;
    }
    ready.set_value();
    int client = ::accept(server, nullptr, nullptr);
    if (client < 0) {
        ::close(server);
        return 4;
    }
    char request[9];
    bool ok = transfer_exact(client, request, sizeof(request), false);
    std::array<unsigned char, 9> response{};
    response[0] = 0x02;
    std::uint64_t length = record.size();
    for (int index = 0; index < 8; ++index) {
        response[1 + index] = static_cast<unsigned char>(length >> (8 * index));
    }
    ok = ok && transfer_exact(client, response.data(), response.size(), true) &&
         transfer_exact(client, record.data(), record.size(), true);
    ::close(client);
    ::close(server);
    return ok ? 0 : 5;
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

    std::string record_text = read_file(first_content_file(spool));
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
