#include "neotape/frame_builder.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: make_replay_cases <captured-slice-zero> "
                     "<new-case-dir> <test-secret-key>\n";
        return 2;
    }
    try {
        namespace fs = std::filesystem;
        auto signer = neotape::load_signify_secret_key(argv[3]);
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<std::vector<std::byte>> records;
        std::vector<neotape::FrameHeader> headers;
        std::vector<size_t> content, repair;
        for (;;) {
            neotape::HeaderBytes head{};
            input.read(reinterpret_cast<char *>(head.data()), head.size());
            if (input.gcount() == 0 && input.eof())
                break;
            if (input.gcount() != static_cast<std::streamsize>(head.size()))
                throw std::runtime_error("short header");
            auto header = neotape::parse_fixed_header(head.data(), head.size());
            if (header.slice_seq_num != 0)
                throw std::runtime_error(
                    "fixture must be first complete slice");
            std::vector<std::byte> record(neotape::decoded_block_size(header));
            std::memcpy(record.data(), head.data(), head.size());
            input.read(reinterpret_cast<char *>(record.data() + head.size()),
                       record.size() - head.size());
            if (!input)
                throw std::runtime_error("short record");
            if (header.channel_type == neotape::ChannelType::CH_CONTENT)
                content.push_back(records.size());
            if (header.channel_type == neotape::ChannelType::CH_FEC)
                repair.push_back(records.size());
            headers.push_back(header);
            records.push_back(std::move(record));
        }
        if (content.size() < 32 || repair.size() < 4 ||
            !neotape::has_frame_flag_end(headers[content.back()].flags) ||
            !neotape::has_frame_flag_end(headers[repair.back()].flags))
            throw std::runtime_error("fixture lacks a complete FEC slice");
        fs::path root(argv[2]);
        if (!fs::create_directory(root))
            throw std::runtime_error("case directory exists");
        auto const &last = headers.back();
        auto end = neotape::build_archive_end_record(
            neotape::decoded_block_size(last), last.volume_seq_num,
            last.archive_uuid, last.archive_label,
            last.global_frame_seq_num + 1, &signer);
        std::ofstream expected(root / "expected.pax", std::ios::binary);
        for (auto index : content)
            expected.write(
                reinterpret_cast<const char *>(records[index].data()) + 512,
                headers[index].frame_payload_size);
        for (auto name :
             {"baseline", "missing-content", "missing-repair", "missing-both",
              "missing-final-repair", "too-many-missing", "damaged-header",
              "bad-signature", "conflicting-replay"}) {
            std::string scenario(name);
            auto directory = root / scenario;
            fs::create_directory(directory);
            std::set<size_t> dropped;
            if (scenario == "missing-content" || scenario == "missing-both")
                dropped.insert(content[1]);
            if (scenario == "missing-repair" || scenario == "missing-both")
                dropped.insert(repair[0]);
            if (scenario == "missing-final-repair")
                dropped.insert(repair.back());
            if (scenario == "too-many-missing")
                dropped.insert(content.begin(), content.begin() + 5);
            std::ofstream output(directory / "neotape-000000.slice-000000.nts",
                                 std::ios::binary);
            for (size_t i = 0; i < records.size(); ++i) {
                if (dropped.contains(i))
                    continue;
                auto record = records[i];
                if (i == content[1] && scenario == "damaged-header")
                    record[0] ^= std::byte{1};
                if (i == content[1] && scenario == "bad-signature")
                    record[416] ^= std::byte{1};
                output.write(reinterpret_cast<const char *>(record.data()),
                             record.size());
                if (i == content[1] && scenario == "conflicting-replay") {
                    record[512] ^= std::byte{1};
                    auto header = headers[i];
                    neotape::finalize_record(header, record, &signer);
                    output.write(reinterpret_cast<const char *>(record.data()),
                                 record.size());
                }
            }
            std::ofstream ending(directory / "neotape-000001.archive-end.nts",
                                 std::ios::binary);
            ending.write(reinterpret_cast<const char *>(end.data()),
                         end.size());
        }
        std::cout << "cases=9 records=" << records.size() << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
