#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/signature.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/socket.h>

int main(int argc, char **argv) {
    if (argc != 5) {
        std::cerr << "usage: capture_volume <tape:path|spool:path> "
                     "<extractor-address> <new-capture-dir> <public-key>\n";
        return 2;
    }
    try {
        namespace fs = std::filesystem;
        using neotape::tcp::MessageType;
        fs::path root(argv[3]);
        if (!fs::create_directory(root))
            throw std::runtime_error(
                "capture directory must not already exist");
        std::ofstream events(root / "records.tsv");
        events << "event\tfile\tglobal\tslice\tchannel\tchannel_"
                  "seq\tbytes\thash\n";
        std::ofstream frames, prefix(root / "prefix.bin", std::ios::binary);
        uint64_t file = UINT64_MAX, count = 0, bytes = 0, marks = 0, last = 0;
        bool saw_frame = false, ended = false;
        neotape::FdGuard socket(neotape::connect_to_server(argv[2]));
        timeval timeout{120, 0};
        if (::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)) ||
            ::setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout)))
            throw std::runtime_error("socket timeout configuration failed");
        auto key = neotape::load_signify_public_key(argv[4]);
        neotape::RecordReader reader(neotape::parse_media(argv[1]));
        for (;;) {
            auto record = reader.next();
            if (record.event == neotape::RecordEvent::end)
                break;
            if (record.event == neotape::RecordEvent::filemark) {
                ++marks;
                events << "filemark\t" << record.file_num << "\n";
                frames.close();
                file = UINT64_MAX;
                continue;
            }
            auto const *data =
                reinterpret_cast<const uint8_t *>(record.record.data());
            neotape::FrameHeader header;
            try {
                header =
                    neotape::parse_fixed_header(data, record.record.size());
            } catch (...) {
                if (saw_frame)
                    throw;
                prefix.write(reinterpret_cast<const char *>(data),
                             record.record.size());
                if (!prefix)
                    throw std::runtime_error("capture prefix write failed");
                continue;
            }
            if (ended)
                throw std::runtime_error("unexpected record after archive_end");
            saw_frame = true;
            if (!neotape::verify_frame_hash(data, record.record.size(),
                                            header.frame_hash))
                throw std::runtime_error("readback frame hash mismatch");
            auto signature =
                neotape::validate_frame_signature(header, {key}, true);
            if (signature.error)
                throw std::runtime_error(*signature.error);
            if (file != record.file_num) {
                if (frames.is_open())
                    frames.close();
                frames.clear();
                auto type =
                    header.channel_type == neotape::ChannelType::ARCHIVE_END
                        ? std::string("archive-end")
                        : "slice-" + std::to_string(header.slice_seq_num);
                frames.open(root /
                                ("neotape-" + std::to_string(record.file_num) +
                                 "." + type + ".nts"),
                            std::ios::binary);
                file = record.file_num;
            }
            frames.write(reinterpret_cast<const char *>(data),
                         record.record.size());
            if (!frames)
                throw std::runtime_error("capture record write failed");
            auto request = neotape::tcp::read_message(socket.fd);
            if (!request || request->type != MessageType::next_frame)
                throw std::runtime_error(
                    "extractor did not request next record");
            neotape::tcp::write_message(
                socket.fd, {MessageType::frame_record, record.record});
            auto ack = neotape::tcp::read_message(socket.fd);
            if (!ack || ack->type != MessageType::ack_frame ||
                ack->payload.size() != 8 ||
                neotape::le64_from_bytes(ack->payload) !=
                    header.global_frame_seq_num)
                throw std::runtime_error(
                    "extractor rejected record or returned wrong ACK");
            ended = header.channel_type == neotape::ChannelType::ARCHIVE_END;
            events << "record\t" << file << '\t' << header.global_frame_seq_num
                   << '\t' << header.slice_seq_num << '\t'
                   << unsigned(header.channel_type) << '\t'
                   << header.channel_frame_seq_num << '\t'
                   << record.record.size() << '\t'
                   << neotape::hash_hex(header.frame_hash) << '\n';
            ++count;
            bytes += record.record.size();
            last = header.global_frame_seq_num;
            if (count % 1024 == 0) {
                events.flush();
                std::cerr << "captured_records=" << count << " bytes=" << bytes
                          << '\n';
            }
        }
        if (!ended) {
            auto request = neotape::tcp::read_message(socket.fd);
            if (!request || request->type != MessageType::next_frame)
                throw std::runtime_error(
                    "extractor did not request volume continuation");
            neotape::tcp::write_message(socket.fd, {MessageType::tape_eof, {}});
        }
        frames.flush();
        events.flush();
        std::cout << "records=" << count << " bytes=" << bytes
                  << " filemarks=" << marks << " last_global=" << last
                  << " archive_end=" << ended << '\n';
    } catch (const std::exception &error) {
        std::cerr << "capture_volume: " << error.what() << '\n';
        return 1;
    }
}
