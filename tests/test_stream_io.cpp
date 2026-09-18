#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/plan.hpp"
#include "support/temp_directory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using neotape::test::TemporaryDirectory;

std::string frame_bytes() {
    neotape::FrameHeader header;
    header.channel_type = neotape::ChannelType::CH_CONTENT;
    header.volume_block_size_kib = 4;
    header.archive_uuid = "00000000-0000-4000-8000-000000000123";
    header.flags = neotape::frame_flag_end;
    auto serialized = neotape::serialize_frame_header(header);
    std::string record(4096, '\0');
    std::copy(serialized.begin(), serialized.end(), record.begin());
    return record;
}
} // namespace

TEST_CASE(
    "spool reader preserves file boundaries and rejects truncated records",
    "[unit][media]") {
    TemporaryDirectory temporary;
    auto bytes = frame_bytes();
    std::ofstream(temporary.path() / "neotape-000002.slice-000000.nts",
                  std::ios::binary)
        << bytes << bytes;
    std::ofstream(temporary.path() / "neotape-000010.slice-000000.nts",
                  std::ios::binary)
        << bytes.substr(0, 600);
    neotape::RecordReader reader(
        {neotape::MediaLocator::spool, temporary.path().string()});
    for (int i = 0; i < 2; ++i) {
        auto record = reader.next();
        REQUIRE(record.event == neotape::RecordEvent::record);
        REQUIRE(record.file_num == 2);
        REQUIRE(record.record.size() == bytes.size());
    }
    auto mark = reader.next();
    REQUIRE(mark.event == neotape::RecordEvent::filemark);
    REQUIRE(mark.file_num == 2);
    REQUIRE_THROWS(reader.next());
}

TEST_CASE("first-record scan skips an unreadable spool tail", "[unit][media]") {
    TemporaryDirectory temporary;
    auto bytes = frame_bytes();
    std::ofstream(temporary.path() / "neotape-000000.slice-000000.nts",
                  std::ios::binary)
        << bytes << "damaged tail";
    std::ofstream(temporary.path() / "neotape-000001.slice-000000.nts",
                  std::ios::binary)
        << bytes;
    neotape::RecordReader reader(
        {neotape::MediaLocator::spool, temporary.path().string()});
    REQUIRE(reader.next().event == neotape::RecordEvent::record);
    reader.skip_file();
    auto record = reader.next();
    REQUIRE(record.event == neotape::RecordEvent::record);
    REQUIRE(record.file_num == 1);
    REQUIRE(reader.next().event == neotape::RecordEvent::filemark);
    REQUIRE(reader.next().event == neotape::RecordEvent::end);
}

TEST_CASE("plan reader consumes opaque records incrementally and rejects a "
          "torn trailer",
          "[unit][plan]") {
    TemporaryDirectory temporary;
    auto path = temporary.path() / "plan";
    std::string record = "/0/0/f/0/0/0/root/0/root/dir/opaque-\x82\xe7\nfile";
    {
        std::ofstream output(path, std::ios::binary);
        output << record << '\0' << '\n' << "/chdir/incomplete";
    }
    neotape::PlanReader reader(path);
    auto first = reader.next();
    REQUIRE(first.has_value());
    REQUIRE(first->entry.has_value());
    REQUIRE(first->entry->path == "dir/opaque-\x82\xe7\nfile");
    REQUIRE_THROWS(reader.next());
}

TEST_CASE("spool rejects duplicate numeric file numbers before playback",
          "[unit][media]") {
    TemporaryDirectory temporary;
    std::ofstream(temporary.path() / "neotape-000001.slice-000000.nts")
        << frame_bytes();
    std::ofstream(temporary.path() / "neotape-1.archive-end.nts")
        << frame_bytes();
    REQUIRE_THROWS(neotape::RecordReader(
        {neotape::MediaLocator::spool, temporary.path().string()}));
}

TEST_CASE(
    "plan reader checks signed epochs and rejects invalid numeric domains",
    "[unit][plan]") {
    auto const mtime = GENERATE(std::string("-9223372036854775808"),
                                std::string("9223372036854775807"));
    TemporaryDirectory temporary;
    auto path = temporary.path() / "plan";
    std::ofstream(path, std::ios::binary)
        << "/0/0/f/0/" << mtime << "/0//0//file" << '\0' << '\n';
    neotape::PlanReader reader(path);
    REQUIRE(std::to_string(reader.next()->entry->mtime) == mtime);
}

TEST_CASE("plan reader rejects overflow negative IDs and unknown kinds",
          "[unit][plan]") {
    auto const record =
        GENERATE("/0/0/f/0/9223372036854775808/0//0//file",
                 "/0/0/f/0/0/4294967296//0//file", "/0/0/f/0/0/-1//0//file",
                 "/0/0/f/18446744073709551616/0/0//0//file",
                 "/0/0/x/0/0/0//0//file", "/0/1/f/0/0/0//0//file");
    TemporaryDirectory temporary;
    auto path = temporary.path() / "plan";
    std::ofstream(path, std::ios::binary) << record << '\0' << '\n';
    neotape::PlanReader reader(path);
    REQUIRE_THROWS(reader.next());
}

TEST_CASE("spool filename grammar rejects overflow and ignores unrelated files",
          "[unit][media]") {
    uint64_t number = 0;
    REQUIRE(neotape::parse_spool_file_name(
        "neotape-00012.slice-000003-copy_1.nts", number));
    REQUIRE(number == 12);
    REQUIRE_FALSE(
        neotape::parse_spool_file_name("neotape-12.unknown.nts", number));
    REQUIRE_FALSE(
        neotape::parse_spool_file_name("neotape-12.slice-.nts", number));
    REQUIRE_THROWS(neotape::parse_spool_file_name(
        "neotape-18446744073709551616.archive-end.nts", number));
}
