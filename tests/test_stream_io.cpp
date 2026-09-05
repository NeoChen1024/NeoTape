#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/plan.hpp"
#include "support/temp_directory.hpp"
#include <catch2/catch_test_macros.hpp>
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
    std::ofstream(temporary.path() / "neotape-000002.dump.nts",
                  std::ios::binary)
        << bytes << bytes;
    std::ofstream(temporary.path() / "neotape-000010.dump.nts",
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
    std::ofstream(temporary.path() / "neotape-000000.dump.nts",
                  std::ios::binary)
        << bytes << "damaged tail";
    std::ofstream(temporary.path() / "neotape-000001.dump.nts",
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
