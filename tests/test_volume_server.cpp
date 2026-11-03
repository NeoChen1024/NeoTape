#include "neotape/socket_util.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/volume_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using neotape::tcp::Message;
using neotape::tcp::MessageType;

// A connected client/server pair; the client side is written up front so
// serving runs synchronously on the test thread.
struct SocketPair {
    int fds[2];
    SocketPair() { REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0); }
    ~SocketPair() {
        ::close(fds[0]);
        ::close(fds[1]);
    }
    SocketPair(const SocketPair &) = delete;
    SocketPair &operator=(const SocketPair &) = delete;
    int client() const { return fds[0]; }
    int server() const { return fds[1]; }
};

neotape::VolumeServerOptions options(std::string archive_name) {
    neotape::VolumeServerOptions opts;
    opts.volume_block_size = 4096;
    opts.archive_name = std::move(archive_name);
    opts.log_label = "test";
    return opts;
}

neotape::VolumeServeResult serve(const neotape::VolumeServerOptions &opts,
                                 int server,
                                 neotape::VolumeRecordQueue &queue) {
    neotape::VolumeServeState state;
    neotape::FrameRetentionBuffer retention(opts.retention_frame_count);
    return neotape::serve_volume_client(
        server, opts, "00000000-0000-4000-8000-000000000123", state, retention,
        queue, [] { return std::string(); });
}
} // namespace

TEST_CASE("volume server ends only the connection on a peer failure",
          "[unit][server]") {
    SocketPair sockets;
    neotape::VolumeRecordQueue queue;
    // A message type the protocol does not define.
    uint8_t const garbage[9] = {0x7f, 0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE(::write(sockets.client(), garbage, sizeof garbage) ==
            sizeof garbage);
    auto const result = serve(options("archive"), sockets.server(), queue);
    REQUIRE_FALSE(result.archive_complete);
    REQUIRE_FALSE(result.volume_committed);
}

TEST_CASE("volume server propagates a local fault instead of retrying",
          "[unit][server]") {
    SocketPair sockets;
    neotape::VolumeRecordQueue queue;
    // The producer is done; serving builds archive_end, whose header
    // cannot carry an over-long label.
    neotape::push_record(queue, neotape::VolumeRecord{.record = {},
                                                      .global_seq_num = 0,
                                                      .tape_eof = false,
                                                      .done = true});
    neotape::tcp::write_message(sockets.client(),
                                Message{MessageType::next_frame, {}});
    REQUIRE_THROWS_WITH(
        serve(options(std::string(100, 'x')), sockets.server(), queue),
        Catch::Matchers::ContainsSubstring("archive label"));
}
