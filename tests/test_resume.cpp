#include "neotape/resume.hpp"
#include "support/temp_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using namespace std::string_literals;
using neotape::read_resume_file;
using neotape::test::TemporaryDirectory;

// Written out byte for byte: the file format is a compatibility contract.
const std::string header_bytes =
    "/neotape-resume/1\0\n"
    "/archive_uuid/3f2c9d1e-5b7a-4c1e-9f60-2a8d4e6b1c35\0\n"
    "/block_size_kib/1024\0\n"
    "/sign_key_id/9a41c7e2b05d3f18\0\n"
    "/plan_blake3/"
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\0\n"
    "/archive_name/home 2026/full\nline two\0\n"s;

void write_file(const fs::path &path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

std::string read_file(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), {}};
}

} // namespace

TEST_CASE("resume file header and slice starts are read from fixed bytes",
          "[unit][resume]") {
    TemporaryDirectory temporary;
    fs::path const path = temporary.path() / "resume";
    write_file(path, header_bytes + "/slice/1/5120/1\0\n"s +
                         "/slice/2/10311/1\0\n"s + "/slice/3/15540/2\0\n"s);

    auto const state = read_resume_file(path);
    REQUIRE(state.header.archive_uuid ==
            "3f2c9d1e-5b7a-4c1e-9f60-2a8d4e6b1c35");
    REQUIRE(state.header.block_size_kib == 1024);
    REQUIRE(state.header.sign_key_id == "9a41c7e2b05d3f18");
    REQUIRE(state.header.plan_blake3.size() == 64);
    // The name is raw bytes: '/', space and newline survive.
    REQUIRE(state.header.archive_name == "home 2026/full\nline two");
    REQUIRE(state.points.size() == 3);
    REQUIRE(state.points[2].slice == 3);
    REQUIRE(state.points[2].global_seq == 15540);
    REQUIRE(state.points[2].volume == 2);
    REQUIRE(state.valid_size == fs::file_size(path));
}

TEST_CASE("resume header encoding matches the fixed bytes", "[unit][resume]") {
    REQUIRE(neotape::encode_resume_header({
                .archive_uuid = "3f2c9d1e-5b7a-4c1e-9f60-2a8d4e6b1c35",
                .block_size_kib = 1024,
                .sign_key_id = "9a41c7e2b05d3f18",
                .plan_blake3 = "00112233445566778899aabbccddeeff"
                               "00112233445566778899aabbccddeeff",
                .archive_name = "home 2026/full\nline two",
            }) == header_bytes);
    REQUIRE(neotape::encode_resume_point({7, 123456, 3}) ==
            "/slice/7/123456/3\0\n"s);
}

TEST_CASE("a later slice record supersedes that slice and all after it",
          "[unit][resume]") {
    TemporaryDirectory temporary;
    fs::path const path = temporary.path() / "resume";
    write_file(path, header_bytes + "/slice/1/100/1\0\n"s +
                         "/slice/2/200/1\0\n"s + "/slice/3/300/2\0\n"s +
                         "/slice/2/200/4\0\n"s);

    auto const state = read_resume_file(path);
    REQUIRE(state.points.size() == 2);
    REQUIRE(state.points.back().slice == 2);
    REQUIRE(state.points.back().volume == 4);
}

TEST_CASE("only the final record of a resume file may be cut short",
          "[unit][resume]") {
    TemporaryDirectory temporary;
    fs::path const path = temporary.path() / "resume";
    std::string const complete = header_bytes + "/slice/1/100/1\0\n"s;

    for (std::string const &tail :
         {"/slice/2/2"s, "/slice/2/200/1"s, "/slice/2/200/1\0"s}) {
        CAPTURE(tail.size());
        write_file(path, complete + tail);
        auto const state = read_resume_file(path);
        REQUIRE(state.points.size() == 1);
        REQUIRE(state.valid_size == complete.size());
    }

    // The same damage followed by another record is not a crash artefact.
    write_file(path, complete + "/slice/2/200/1\0"s + "/slice/3/300/1\0\n"s);
    REQUIRE_THROWS(read_resume_file(path));
}

TEST_CASE("malformed resume files are rejected", "[unit][resume]") {
    TemporaryDirectory temporary;
    fs::path const path = temporary.path() / "resume";
    std::string const cases[] = {
        "/neotape-resume/2\0\n"s + header_bytes.substr(19),
        header_bytes.substr(0, header_bytes.find("/sign_key_id")),
        header_bytes + "/archive_name/again\0\n"s,
        header_bytes + "/unknown/value\0\n"s,
        header_bytes + "/slice/0/0/1\0\n"s,
        header_bytes + "/slice/1/100\0\n"s,
        header_bytes + "/slice/1/100/1\0\n"s + "/slice/3/300/1\0\n"s,
        header_bytes + "/slice/1/100/1\0\n"s + "/slice/2/100/1\0\n"s,
        header_bytes + "/slice/1/100/2\0\n"s + "/slice/2/200/1\0\n"s,
        header_bytes + "/slice/1/+100/1\0\n"s,
    };
    for (size_t index = 0; index < std::size(cases); ++index) {
        CAPTURE(index);
        write_file(path, cases[index]);
        REQUIRE_THROWS(read_resume_file(path));
    }
}

TEST_CASE("resume log refuses to replace a file and appends past a torn tail",
          "[unit][resume]") {
    TemporaryDirectory temporary;
    fs::path const path = temporary.path() / "resume";
    neotape::ResumeHeader const header{
        .archive_uuid = "3f2c9d1e-5b7a-4c1e-9f60-2a8d4e6b1c35",
        .block_size_kib = 64,
        .sign_key_id = {},
        .plan_blake3 = {},
        .archive_name = "a",
    };
    {
        neotape::ResumeLog log(path, header);
        log.slice_started(1, 10);
        log.slice_started(2, 20);
        // Nothing is recorded until the slice's first frame is acknowledged.
        log.frame_acked(9, 1);
        REQUIRE(read_resume_file(path).points.empty());
        log.frame_acked(10, 1);
        log.frame_acked(25, 2);
    }
    REQUIRE_THROWS(neotape::ResumeLog(path, header));

    std::string const recorded = read_file(path);
    REQUIRE(recorded == neotape::encode_resume_header(header) +
                            "/slice/1/10/1\0\n"s + "/slice/2/20/2\0\n"s);

    write_file(path, recorded + "/slice/3/3"s);
    auto const state = read_resume_file(path);
    {
        neotape::ResumeLog log(path, state, state.points.front());
        // The restart slice itself is already recorded.
        log.slice_started(1, 10);
        log.slice_started(2, 21);
        log.frame_acked(21, 5);
    }
    REQUIRE(read_file(path) ==
            recorded + "/slice/1/10/1\0\n"s + "/slice/2/21/5\0\n"s);
    auto const resumed = read_resume_file(path);
    REQUIRE(resumed.points.size() == 2);
    REQUIRE(resumed.points.back().global_seq == 21);
}
