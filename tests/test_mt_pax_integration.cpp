#include "neotape/pax_writer.hpp"
#include "support/checks.hpp"
#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <memory>
#include <thread>

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

using neotape::test::require_success;

TEST_CASE(
    "planned pax preserves contents while progress samples slice turnover",
    "[integration][pax][plan]") {
    TemporaryDirectory temporary;
    fs::path const plan_path = temporary.path() / "plan";
    std::ofstream plan(plan_path, std::ios::binary);
    constexpr unsigned slice_count = 64;
    for (unsigned i = 0; i < slice_count; ++i) {
        fs::path const source = temporary.path() / std::to_string(i);
        std::string const contents = "slice contents " + std::to_string(i);
        std::ofstream(source) << contents;
        plan << '/' << i << "/0/f/" << contents.size() << "/0/0/root/0/root/"
             << source.string() << '\0' << '\n';
    }
    plan.close();

    neotape::PaxWriterOptions options;
    options.plan_path = plan_path;
    options.output_buf_size = 4096;
    options.io_thread = 4;
    std::vector<std::byte> bytes;
    neotape::PaxWriterCallbacks callbacks;
    callbacks.write_chunk = [&](neotape::PaxChunk chunk) {
        bytes.insert(bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
        // Keep slice turnover active across several periodic stats samples.
        std::this_thread::sleep_for(25ms);
    };
    auto const result = neotape::write_pax(options, std::move(callbacks));
    REQUIRE(result.slices == slice_count);

    std::unique_ptr<archive, decltype(&archive_read_free)> reader(
        archive_read_new(), archive_read_free);
    REQUIRE(reader != nullptr);
    REQUIRE(archive_read_support_format_tar(reader.get()) == ARCHIVE_OK);
    REQUIRE(archive_read_open_memory(reader.get(), bytes.data(),
                                     bytes.size()) == ARCHIVE_OK);
    archive_entry *entry = nullptr;
    for (unsigned i = 0; i < slice_count; ++i) {
        REQUIRE(archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK);
        std::string const expected = "slice contents " + std::to_string(i);
        std::string restored(expected.size(), '\0');
        REQUIRE(
            archive_read_data(reader.get(), restored.data(), restored.size()) ==
            static_cast<la_ssize_t>(expected.size()));
        REQUIRE(restored == expected);
    }
    REQUIRE(archive_read_next_header(reader.get(), &entry) == ARCHIVE_EOF);
}

TEST_CASE("mt-pax defers file opens under descriptor pressure",
          "[integration][pax][resource-limit]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "source";
    fs::path const fifo = temporary.path() / "archive.pipe";
    fs::create_directory(source);
    for (int index = 0; index < 60; ++index) {
        fs::path const file =
            source / ("file-" + std::to_string(index) + ".bin");
        {
            std::ofstream stream(file);
        }
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
    REQUIRE(writer_result.standard_error.find("Can't open") ==
            std::string::npos);
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
        fs::path const directory =
            directories / ("dir-" + std::to_string(index));
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

    ProcessResult const extract_result =
        Process::run(ProcessOptions{{NEOTAPE_BSDTAR, "-xpf", archive.string(),
                                     "-C", output.string()}},
                     30s);
    require_success(extract_result);

    std::ifstream expected(small / "file-17.txt");
    std::ifstream actual(output / "src/small/file-17.txt");
    REQUIRE(std::string(std::istreambuf_iterator<char>(expected), {}) ==
            std::string(std::istreambuf_iterator<char>(actual), {}));
    REQUIRE(fs::is_symlink(output / "src/dirs/dir-17/link-17"));
}

TEST_CASE("mt-pax preserves opaque pathname and symlink target bytes",
          "[integration][pax][filename]") {
    TemporaryDirectory temporary;
    fs::path const source = temporary.path() / "src";
    fs::path const archive = temporary.path() / "planned-000000.pax";
    fs::path const output = temporary.path() / "out";
    fs::create_directory(source);
    fs::create_directory(output);

    std::string const opaque_name = "opaque-\x82\xe7\n.bin";
    fs::path const opaque_path = source / fs::path(opaque_name);
    std::ofstream(opaque_path) << "opaque filename payload\n";
    fs::create_symlink(fs::path(opaque_name), source / "opaque-link");

    fs::path const plan = temporary.path() / "plan";
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_PLAN, "-C", temporary.path().string(), "-o",
                        plan.string(), "src"}},
        30s));
    ProcessResult const archive_result = Process::run(
        ProcessOptions{{NEOTAPE_MT_PAX, "-v", "--slice-output-prefix",
                        (temporary.path() / "planned-").string(), "--plan",
                        plan.string()}},
        30s);
    require_success(archive_result);
    REQUIRE(archive_result.standard_error.find("opaque-\\x82\\xe7\\x0a.bin") !=
            std::string::npos);
    REQUIRE(archive_result.standard_error.find(opaque_name) ==
            std::string::npos);

    ProcessResult const extract_result =
        Process::run(ProcessOptions{{NEOTAPE_BSDTAR, "-xpf", archive.string(),
                                     "-C", output.string()}},
                     30s);
    require_success(extract_result);

    fs::path const extracted_file = output / "src" / fs::path(opaque_name);
    REQUIRE(fs::is_regular_file(extracted_file));
    std::ifstream payload(extracted_file);
    REQUIRE(std::string(std::istreambuf_iterator<char>(payload), {}) ==
            "opaque filename payload\n");
    REQUIRE(fs::read_symlink(output / "src/opaque-link").native() ==
            opaque_name);
}
