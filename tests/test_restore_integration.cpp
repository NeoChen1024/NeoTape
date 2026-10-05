#include "support/checks.hpp"
#include "support/process.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
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
                   const fs::path &output, const fs::path &metadata = {}) {
    std::vector<std::string> command{NEOTAPE_EXTRACTOR, "--listen",
                                     "unix://" + socket.string(), "-o",
                                     output.string()};
    if (!metadata.empty()) {
        command.emplace_back("--metadata-output");
        command.push_back(metadata.string());
    }
    Process extractor(ProcessOptions{command});
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
    bool const write_catalog = GENERATE(true, false);
    CAPTURE(write_catalog);
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
         plan.string(), "--output-buffer-size", "1M", "-P", "80",
         "--volume-block-size", "64K", "--plan-write-mode",
         write_catalog ? "slice0" : "no"}});
    REQUIRE(wait_for_unix_socket(archiver_socket, archiver, 5s));
    require_success(Process::run(
        ProcessOptions{{NEOTAPE_WRITE, "--source",
                        "unix://" + archiver_socket.string(), "--target",
                        "spool:" + spool.string(), "--erase"}},
        60s));
    require_success(archiver.wait(60s));
    fs::path const catalog = temporary.path() / "catalog";
    restore_spool(extractor_socket, spool, restored, catalog);

    // The catalog is the plan file itself, /chdir/ record included.
    REQUIRE(fs::file_size(plan) > 0);
    REQUIRE(read_file(catalog) ==
            (write_catalog ? read_file(plan) : std::string{}));

    ProcessResult const preview =
        Process::run(ProcessOptions{{NEOTAPE_CATALOG, "--source",
                                     "spool:" + spool.string()}},
                     30s);
    ProcessResult const listing =
        Process::run(ProcessOptions{{NEOTAPE_CATALOG, "--source",
                                     "spool:" + spool.string(), "--list"}},
                     30s);
    if (write_catalog) {
        require_success(preview);
        REQUIRE(preview.standard_output == read_file(plan));
        require_success(listing);
        // One line per planned entry; the /chdir/ record is not listed.
        REQUIRE(std::ranges::count(listing.standard_output, '\n') == 2);
        REQUIRE(listing.standard_output.find("hello.txt") != std::string::npos);
        REQUIRE(listing.standard_output.find("blob.bin") != std::string::npos);
        REQUIRE(listing.standard_output.find(input.string()) ==
                std::string::npos);
    } else {
        INFO(preview.standard_error);
        REQUIRE(preview.exit_code == 1);
        REQUIRE(preview.standard_output.empty());
        REQUIRE(listing.exit_code == 1);
    }

    fs::path const restored_output = temporary.path() / "restored-output";
    extract_pax(restored, restored_output);
    REQUIRE(read_file(input / "hello.txt") ==
            read_file(restored_output / "hello.txt"));
    REQUIRE(read_file(input / "blob.bin") ==
            read_file(restored_output / "blob.bin"));
}

TEST_CASE("an interrupted plan-driven archive resumes from a slice start",
          "[integration][plan][resume][socket]") {
    // The resume file is either the archiver's own or one rebuilt from media.
    bool const rebuilt_from_media = GENERATE(false, true);
    CAPTURE(rebuilt_from_media);
    TemporaryDirectory temporary;
    fs::path const input = temporary.path() / "input";
    fs::path const plan = temporary.path() / "plan";
    fs::path const spool = temporary.path() / "spool";
    fs::path const resume = temporary.path() / "resume";
    fs::path const rebuilt = temporary.path() / "rebuilt";
    fs::path const restored = temporary.path() / "restored.pax";
    fs::path const archiver_socket = temporary.path() / "archiver.sock";
    fs::path const extractor_socket = temporary.path() / "extractor.sock";
    fs::create_directory(input);
    std::vector<std::string> names;
    for (int i = 0; i < 6; ++i) {
        names.push_back("file" + std::to_string(i) + ".bin");
        write_pattern(input / names.back(), 1024 * 1024 + 4096 * i);
    }
    // Planned last, so the link lands in a resumed slice while its target
    // stays in slice 0, which the resumed run does not emit.
    fs::create_hard_link(input / "file0.bin", input / "link.bin");
    names.emplace_back("link.bin");

    std::vector<std::string> plan_command{
        NEOTAPE_PLAN,   "-C", input.string(), "-o", plan.string(),
        "--slice-size", "1M"};
    plan_command.insert(plan_command.end(), names.begin(), names.end());
    require_success(Process::run(ProcessOptions{plan_command}, 60s));

    std::vector<std::string> const archive_command{NEOTAPE_ARCHIVER,
                                                   "--listen",
                                                   "unix://" +
                                                       archiver_socket.string(),
                                                   "--plan",
                                                   plan.string(),
                                                   "--volume-block-size",
                                                   "64K",
                                                   "--archive-name",
                                                   "resume/test",
                                                   "--resume-file"};
    {
        auto command = archive_command;
        command.push_back(resume.string());
        Process archiver{ProcessOptions{command}};
        REQUIRE(wait_for_unix_socket(archiver_socket, archiver, 5s));
        // The capacity limit stands in for the failure: it stops the writer
        // inside a slice well before the archive ends.
        ProcessResult const writer =
            Process::run(ProcessOptions{{NEOTAPE_WRITE, "--source",
                                         "unix://" + archiver_socket.string(),
                                         "--target", "spool:" + spool.string(),
                                         "--max-volume-bytes", "3600K"}},
                         60s);
        INFO(writer.standard_error);
        REQUIRE(writer.exit_code == 3);
        archiver.terminate();
    }
    fs::remove(archiver_socket);

    // A second archive must not reuse the file by accident.
    {
        auto command = archive_command;
        command.push_back(resume.string());
        ProcessResult const refused =
            Process::run(ProcessOptions{command}, 30s);
        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.standard_error.find("already exists") !=
                std::string::npos);
    }

    require_success(Process::run(
        ProcessOptions{{NEOTAPE_SCAN, "--source", "spool:" + spool.string(),
                        "--resume-output", rebuilt.string()}},
        30s));
    // Both sources must agree on where every slice starts.
    std::string const recorded = read_file(resume);
    std::string const scanned = read_file(rebuilt);
    size_t const recorded_slices = recorded.find("/slice/");
    size_t const scanned_slices = scanned.find("/slice/");
    REQUIRE(recorded_slices != std::string::npos);
    REQUIRE(scanned_slices != std::string::npos);
    REQUIRE(recorded.substr(recorded_slices) == scanned.substr(scanned_slices));

    // Position the medium: drop the slice that was being written.
    std::string const last_record =
        recorded.substr(recorded.rfind("/slice/") + 7);
    uint64_t const resume_slice =
        std::stoull(last_record.substr(0, last_record.find('/')));
    REQUIRE(resume_slice >= 2);
    REQUIRE(resume_slice < 6);
    std::string const partial_name = std::format(".slice-{:06}.", resume_slice);
    size_t removed = 0;
    for (const auto &entry : fs::directory_iterator(spool)) {
        if (entry.path().filename().string().find(partial_name) !=
            std::string::npos) {
            fs::remove(entry.path());
            ++removed;
        }
    }
    REQUIRE(removed == 1);

    {
        auto command = archive_command;
        command.push_back((rebuilt_from_media ? rebuilt : resume).string());
        command.insert(command.end(), {"--resume-from-slice", "last"});
        Process archiver{ProcessOptions{command}};
        REQUIRE(wait_for_unix_socket(archiver_socket, archiver, 5s));
        require_success(Process::run(
            ProcessOptions{{NEOTAPE_WRITE, "--source",
                            "unix://" + archiver_socket.string(), "--target",
                            "spool:" + spool.string(), "--resume-mode"}},
            60s));
        require_success(archiver.wait(60s));
    }

    ProcessResult const inspect =
        Process::run(ProcessOptions{{NEOTAPE_INSPECT, "--source",
                                     "spool:" + spool.string()}},
                     30s);
    require_success(inspect);
    REQUIRE((inspect.standard_output + inspect.standard_error)
                .find("Compliance: PASS") != std::string::npos);

    restore_spool(extractor_socket, spool, restored);
    fs::path const output = temporary.path() / "restored-output";
    extract_pax(restored, output);
    for (const std::string &name : names) {
        CAPTURE(name);
        REQUIRE(read_file(input / name) == read_file(output / name));
    }
    REQUIRE(fs::equivalent(output / "file0.bin", output / "link.bin"));
}
