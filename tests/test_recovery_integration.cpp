#include "neotape/format.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include "support/checks.hpp"
#include "support/process.hpp"
#include "support/temp_directory.hpp"
#include <catch2/generators/catch_generators.hpp>
#include <poll.h>
#include <sys/socket.h>

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

using neotape::test::require_success;

using neotape::test::write_pattern;

using neotape::test::read_file;

using neotape::test::content_file;

void flip_payload_byte(const fs::path &path, std::size_t record_index) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    std::streamoff const offset =
        static_cast<std::streamoff>(record_index * 4096 + 512);
    REQUIRE(stream.is_open());
    stream.seekg(offset);
    char byte = 0;
    stream.read(&byte, 1);
    REQUIRE(stream.good());
    byte ^= 1;
    stream.seekp(offset);
    stream.write(&byte, 1);
    stream.flush();
    REQUIRE(stream.good());
}

void create_raw_spool(const fs::path &input, const fs::path &socket,
                      const fs::path &spool) {
    std::vector<std::string> arguments{NEOTAPE_RAW_STORE,
                                       "--listen",
                                       "unix://" + socket.string(),
                                       "--input",
                                       input.string(),
                                       "--volume-block-size",
                                       "4096",
                                       "--archive-name",
                                       "recovery-test"};
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

TEST_CASE("normal restore rejects a corrupt content record",
          "[integration][recovery][socket]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input.bin";
    fs::path const spool = temporary.path() / "spool";
    constexpr std::size_t capacity = 4096 - 512;
    write_pattern(input, capacity * 3 + 123);
    create_raw_spool(input, temporary.path() / "raw.sock", spool);

    fs::path const intact = temporary.path() / "intact.bin";
    require_success(extract(temporary.path() / "intact.sock", spool, intact));
    REQUIRE(read_file(input) == read_file(intact));

    flip_payload_byte(content_file(spool), 1);
    ProcessResult const corrupt =
        extract(temporary.path() / "corrupt.sock", spool,
                temporary.path() / "corrupt.bin", false, false);
    REQUIRE_FALSE(corrupt.timed_out);
    REQUIRE(corrupt.exit_code != 0);
    REQUIRE(corrupt.standard_error.find("frame hash mismatch") !=
            std::string::npos);
    REQUIRE(corrupt.standard_error.find(
                "unrecoverable frame validation failure") != std::string::npos);
}

TEST_CASE("salvage skips a corrupt frame with warnings",
          "[integration][salvage][socket]") {
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input.bin";
    fs::path const spool = temporary.path() / "spool";
    fs::path const output = temporary.path() / "output.bin";
    constexpr std::size_t capacity = 4096 - 512;
    write_pattern(input, capacity * 2 + 123);
    create_raw_spool(input, temporary.path() / "raw.sock", spool);
    flip_payload_byte(content_file(spool), 1);

    ProcessResult const result =
        extract(temporary.path() / "extract.sock", spool, output, true);
    require_success(result);
    std::string const original = read_file(input);
    std::string expected;
    expected.insert(expected.end(), original.begin(),
                    original.begin() + capacity);
    expected.insert(expected.end(), original.begin() + 2 * capacity,
                    original.end());
    REQUIRE(expected == read_file(output));
    REQUIRE(result.standard_error.find(
                "SALVAGE MODE: output is not fully verified") !=
            std::string::npos);
    REQUIRE(result.standard_error.find(
                "salvage skipped frame: frame hash mismatch") !=
            std::string::npos);
}

TEST_CASE("retry records with new volume hashes are emitted once",
          "[integration][replay][socket]") {
    bool const conflict = GENERATE(false, true);
    TemporaryDirectory temporary;
    auto input = temporary.path() / "input";
    auto spool = temporary.path() / "spool";
    auto output = temporary.path() / "output";
    write_pattern(input, 3584 * 3 + 123);
    create_raw_spool(input, temporary.path() / "store.sock", spool);
    auto file = content_file(spool);
    auto original = read_file(file);
    std::string retry = original.substr(4096, 4096 * 2);
    for (std::size_t offset = 0; offset < retry.size(); offset += 4096) {
        auto *data = reinterpret_cast<uint8_t *>(retry.data() + offset);
        auto header = neotape::parse_fixed_header(data, 4096);
        header.volume_seq_num = 2;
        if (conflict && offset == 0)
            data[512] ^= 1;
        auto encoded = neotape::serialize_frame_header(header);
        std::copy(encoded.begin(), encoded.end(), data);
        header.frame_hash = neotape::compute_frame_hash(data, 4096);
        encoded = neotape::serialize_frame_header(header);
        std::copy(encoded.begin(), encoded.end(), data);
    }
    std::ofstream(file, std::ios::binary | std::ios::trunc)
        << original.substr(0, 4096 * 3) << retry << original.substr(4096 * 3);
    auto restored = extract(temporary.path() / "restore.sock", spool, output,
                            false, !conflict);
    INFO(restored.standard_error);
    REQUIRE_FALSE(restored.timed_out);
    if (conflict) {
        REQUIRE(restored.exit_code != 0);
        REQUIRE(restored.standard_error.find("conflicting replay") !=
                std::string::npos);
    } else {
        require_success(restored);
        REQUIRE(read_file(output) == read_file(input));
        REQUIRE(restored.standard_error.find("suppressed replay") !=
                std::string::npos);
    }
}

TEST_CASE("sequence gaps remain fatal",
          "[integration][recovery][socket]") {
    TemporaryDirectory temporary;
    auto input = temporary.path() / "input";
    auto spool = temporary.path() / "spool";
    write_pattern(input, 3584 * 3);
    create_raw_spool(input, temporary.path() / "store.sock", spool);
    auto file = content_file(spool);
    auto original = read_file(file);
    std::ofstream(file, std::ios::binary | std::ios::trunc)
        << original.substr(0, 4096) << original.substr(8192);
    auto restored = extract(temporary.path() / "restore.sock", spool,
                            temporary.path() / "output", false, false);
    REQUIRE_FALSE(restored.timed_out);
    REQUIRE(restored.exit_code != 0);
}

TEST_CASE("reconnected readers replay a suffix without duplicating output",
          "[integration][replay][socket]") {
    using neotape::tcp::MessageType;
    TemporaryDirectory temporary;
    auto input = temporary.path() / "input";
    auto spool = temporary.path() / "spool";
    auto output = temporary.path() / "output";
    write_pattern(input, 3584 * 33 + 123);
    create_raw_spool(input, temporary.path() / "store.sock", spool);
    std::string records = read_file(content_file(spool));
    for (auto const &entry : fs::directory_iterator(spool))
        if (entry.path().filename().string().find(".archive-end.") !=
            std::string::npos)
            records += read_file(entry.path());
    auto socket = temporary.path() / "extract.sock";
    Process extractor(
        ProcessOptions{{NEOTAPE_EXTRACTOR, "--listen",
                        "unix://" + socket.string(), "-o", output.string()}});
    REQUIRE(wait_for_unix_socket(socket, extractor, 5s));
    auto send_range = [&](std::size_t first, std::size_t end) {
        neotape::FdGuard connection(
            neotape::connect_to_server("unix://" + socket.string()));
        timeval timeout{5, 0};
        REQUIRE(::setsockopt(connection.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                             sizeof(timeout)) == 0);
        REQUIRE(::setsockopt(connection.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                             sizeof(timeout)) == 0);
        for (auto index = first; index < end; ++index) {
            auto request = neotape::tcp::read_message(connection.fd);
            REQUIRE(request.has_value());
            REQUIRE(request->type == MessageType::next_frame);
            auto const *data = reinterpret_cast<const std::byte *>(
                records.data() + index * 4096);
            neotape::tcp::write_message(
                connection.fd,
                {MessageType::frame_record, {data, data + 4096}});
            auto ack = neotape::tcp::read_message(connection.fd);
            REQUIRE(ack.has_value());
            REQUIRE(ack->type == MessageType::ack_frame);
            REQUIRE(neotape::le64_from_bytes(ack->payload) == index);
        }
    };
    send_range(0, 20);
    send_range(18, records.size() / 4096);
    auto result = extractor.wait(10s);
    INFO(result.standard_error);
    require_success(result);
    REQUIRE(read_file(output) == read_file(input));
}

TEST_CASE("writer reports archive end spool finalization failure",
          "[integration][writer][socket]") {
    TemporaryDirectory temporary;
    auto input = temporary.path() / "input";
    auto spool = temporary.path() / "spool";
    auto socket = temporary.path() / "store.sock";
    std::ofstream(input) << "payload";
    fs::create_directories(spool / "neotape-000001.archive-end.nts");
    Process archiver(ProcessOptions{
        {NEOTAPE_RAW_STORE, "--listen", "unix://" + socket.string(), "--input",
         input.string(), "--volume-block-size", "4096"}});
    REQUIRE(wait_for_unix_socket(socket, archiver, 5s));
    auto writer = Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source", "unix://" + socket.string(),
                        "--target", "spool:" + spool.string()}},
        10s);
    INFO(writer.standard_error);
    REQUIRE_FALSE(writer.timed_out);
    REQUIRE(writer.exit_code != 0);
    REQUIRE(writer.exit_code !=
            3); // A failed finalization is not a request for another volume.
    require_success(archiver.wait(10s)); // Final record ACK already arrived.
}

TEST_CASE("reader rejects a truncated or mismatched record ACK",
          "[integration][protocol][socket]") {
    bool const truncated = GENERATE(false, true);
    TemporaryDirectory temporary;
    auto input = temporary.path() / "input";
    auto spool = temporary.path() / "spool";
    std::ofstream(input) << "payload";
    create_raw_spool(input, temporary.path() / "store.sock", spool);
    auto address = "unix://" + (temporary.path() / "receiver.sock").string();
    neotape::FdGuard listener(neotape::create_listener(address));
    Process reader(
        ProcessOptions{{NEOTAPE_READ, "--source", "spool:" + spool.string(),
                        "--connect", address}});
    pollfd event{listener.fd, POLLIN, 0};
    REQUIRE(::poll(&event, 1, 5000) == 1);
    neotape::FdGuard client(::accept(listener.fd, nullptr, nullptr));
    REQUIRE(client.fd >= 0);
    timeval timeout{5, 0};
    REQUIRE(::setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)) == 0);
    REQUIRE(::setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout)) == 0);
    neotape::tcp::write_message(client.fd,
                                {neotape::tcp::MessageType::next_frame, {}});
    auto frame = neotape::tcp::read_message(client.fd);
    REQUIRE(frame.has_value());
    REQUIRE(frame->type == neotape::tcp::MessageType::frame_record);
    auto ack = neotape::uint64_to_le_bytes(99);
    if (truncated)
        ack.resize(7);
    neotape::tcp::write_message(client.fd,
                                {neotape::tcp::MessageType::ack_frame, ack});
    auto result = reader.wait(10s);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code != 0);
}
