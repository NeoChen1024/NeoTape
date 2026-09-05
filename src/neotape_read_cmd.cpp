#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/media.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tape.hpp"
#include "neotape/tcp_protocol.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <optional>
#include <unistd.h>
#include <vector>

namespace {

using neotape::connect_to_server;
using neotape::FdGuard;
using neotape::tcp::Message;
using neotape::tcp::MessageType;
using std::format;
using std::string;
using std::vector;

using SourceLocator = neotape::MediaLocator;
using neotape::parse_media;

struct Options {
    SourceLocator source;
    string connect_address;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-read: {}\n", msg);
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-read: {}\n", msg);
    std::exit(2);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} -s|--source <tape:/dev/nst0|spool:./dir>\n"
        "       -c|--connect <tcp://host:port|unix://path> [-h]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"connect", required_argument, nullptr, 'c'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "s:c:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = parse_media(optarg);
            break;
        case 'c':
            opts.connect_address = optarg;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.source.kind == SourceLocator::none) {
        usage_error("--source is required");
    }
    if (opts.connect_address.empty()) {
        usage_error("--connect is required");
    }

    return opts;
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);

    try {
        Options opts = parse_args(argc, argv);

        neotape::RecordReader reader(opts.source);

        FdGuard const fd_guard(connect_to_server(opts.connect_address));
        int const fd = fd_guard.fd;

        uint64_t record_count = 0;
        uint64_t filemark_count = 0;
        uint64_t skipped_prefix_records = 0;
        bool saw_neotape_frame = false;

        for (;;) {
            auto msg = neotape::tcp::read_message(fd);
            if (!msg) {
                break;
            }

            switch (msg->type) {
            case MessageType::next_frame: {
                neotape::MediaRecord event;
                for (;;) {
                    event = reader.next();
                    if (event.event == neotape::RecordEvent::end)
                        break;
                    if (event.event == neotape::RecordEvent::filemark) {
                        ++filemark_count;
                        continue;
                    }
                    if (saw_neotape_frame)
                        break;
                    if (event.record.size() >= neotape::magic.size() &&
                        std::memcmp(event.record.data(), neotape::magic.data(),
                                    neotape::magic.size()) == 0) {
                        saw_neotape_frame = true;
                        break;
                    }
                    ++skipped_prefix_records;
                }
                if (event.event == neotape::RecordEvent::end) {
                    neotape::tcp::write_message(
                        fd, Message{MessageType::tape_eof, {}});
                    std::cerr << format("neotape-read: volume complete: "
                                        "forwarded_frames={} filemarks={}\n",
                                        record_count, filemark_count);
                    return 0;
                }

                if (skipped_prefix_records != 0 && record_count == 0) {
                    std::cerr << format(
                        "neotape-read: skipped {} non-NeoTape BOT records\n",
                        skipped_prefix_records);
                }

                {
                    vector<std::byte> frame_bytes;
                    frame_bytes.swap(event.record);
                    Message frame_msg;
                    frame_msg.type = MessageType::frame_record;
                    frame_msg.payload.swap(frame_bytes);
                    neotape::tcp::write_message(fd, frame_msg);
                }
                ++record_count;

                auto ack = neotape::tcp::read_message(fd);
                if (!ack) {
                    fail("unexpected disconnect after frame_record");
                }
                if (ack->type != MessageType::ack_frame) {
                    fail(format("expected ack_frame, got message type {}",
                                static_cast<int>(ack->type)));
                }
                break;
            }
            case MessageType::error: {
                string reason;
                reason.reserve(msg->payload.size());
                for (std::byte const b : msg->payload) {
                    reason.push_back(static_cast<char>(b));
                }
                if (reason.empty()) {
                    reason = "extractor reported error";
                }
                fail(reason);
            }
            default:
                fail(format("unexpected message type {}",
                            static_cast<int>(msg->type)));
            }
        }

        std::cerr << format(
            "neotape-read: volume complete: forwarded_frames={} "
            "filemarks={}\n",
            record_count, filemark_count);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
